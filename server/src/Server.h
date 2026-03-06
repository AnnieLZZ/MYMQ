#ifndef SERVER_H
#define SERVER_H

#include "Timer.h"
#include "CONFIG_MANAGER.h"
// #include "MYMQ_innercodes.h" // Removed MYMQ dependency
// #include "MYMQ_Publiccodes.h" // Removed MYMQ dependency
#include "Printqueue.h"
#include "tbb/concurrent_hash_map.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <string.h>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <variant>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "SharedThreadPool.h"
// #include "MYMQ_Server_ns.h" // Removed MYMQ dependency

// Generic Networking Types
namespace Net {

    struct FileSendTask {
        int in_fd;           // File descriptor
        off_t offset;        // Start offset in file
        size_t length;       // Length to send
        size_t sent_so_far;  // Internal tracking

        // Header to send before the file content
        std::vector<unsigned char> header_data;
        size_t header_send_offset = 0;

        // Metadata for callbacks/logging (Generic user data could be added here if needed)
        // For now, we keep it simple. If the user needs to track correlation_id, 
        // they can capture it in the completion callback (future feature).
        
        FileSendTask(int fd, off_t off, size_t len, std::vector<unsigned char> hdr)
            : in_fd(fd), offset(off), length(len), sent_so_far(0), header_data(std::move(hdr)) {}
    };

    class ClientState {
    public:
        enum State {
            READING_HEADER,
            READING_BODY
        };

        // State Management
        State current_state = READING_HEADER;

        // Buffers
        std::vector<unsigned char> header_buffer;
        size_t bytes_read_in_header = 0;

        std::vector<unsigned char> body_buffer;
        size_t bytes_read_in_body = 0;

        // Protocol Fields (Generic)
        uint32_t expected_body_length = 0;
        
        // Added for MYMQ compatibility
        uint16_t event_type = 0;
        uint32_t correlation_id = 0;
        uint16_t ack_level = 0;
        bool id_registered = false;
        
        // These were MYMQ specific, but are common enough for a length-prefixed protocol.
        // We can keep them generic or parse them in the callback.
        // To be fully decoupled, the Server should only care about LENGTH.
        // But for convenience, we store the parsed header fields here if the protocol is fixed.
        // Let's assume the Server supports a pluggable Header Parser, 
        // OR we just expose the raw header buffer to the callback.
        // For this refactor, we will expose raw header to callback.

        std::atomic<bool> is_closing{false};
        int fd = -1;

        std::string clientid = "UNKNOWN"; // Generic ID

        SSL* ssl = nullptr;
        bool is_handshake_complete = false;
        bool enable_sendfile = false;

        bool epoll_out_registered = false; 

        // Send Queue
        using SendItem = std::variant<std::vector<unsigned char>, FileSendTask>;

        std::deque<SendItem> send_queue;
        size_t current_vec_send_offset = 0; 
        bool is_writing = false;
        std::mutex send_queue_mtx; 

        // Callback to notify the event loop that data is ready to be written
        // This is crucial for async sends (e.g. from other threads) to wake up epoll
        std::function<void(int)> on_write_ready;

        ClientState(size_t header_size) : header_buffer(header_size) {}
        ClientState() = default;

        bool is_closed(){ return is_closing.load(); }
        int get_fd(){ return fd; }

        void enqueue_message(std::variant<std::vector<unsigned char>, FileSendTask> payload) {
            std::unique_lock<std::mutex> statelock(this->send_queue_mtx);
            bool was_empty = send_queue.empty();
            send_queue.emplace_back(std::move(payload));
            if (was_empty) {
                is_writing = true;
                if(on_write_ready && fd != -1) {
                     on_write_ready(fd);
                }
            }
        }
    };

    class TcpSession {
    public:
        TcpSession(std::shared_ptr<ClientState> state) : state_(state) {
            if(state) clientid = state->clientid;
        }

        void send(std::vector<unsigned char> msg) {
            auto state = state_.lock();
            if (!state || state->is_closed()) return;
            state->enqueue_message(std::move(msg));
        }

        void send_file(FileSendTask task) {
            auto state = state_.lock();
            if (!state || state->is_closed()) return;
            state->enqueue_message(std::move(task));
        }

        int fd() const {
            auto state = state_.lock();
            return state ? state->get_fd() : -1;
        }

        bool is_connected() const {
            auto state = state_.lock();
            return state && !state->is_closed();
        }
        std::string get_clientid(){
            return clientid;
        }
        
        void set_clientid(const std::string& id) {
            auto state = state_.lock();
            if (state) {
                state->clientid = id;
                clientid = id;
            }
        }

    private:
        std::weak_ptr<ClientState> state_;
        std::string clientid;
    };
}

using Net::ClientState;
using Net::TcpSession;
using Net::FileSendTask;

// Helper for time string (keep as is)
std::string now_ms_time_gen_str(); // (Forward declaration, impl below or separate)

using ClientStateMap = tbb::concurrent_hash_map<int, std::shared_ptr<ClientState>>;

class Server {
public:
    enum class IOStatus {
        OK_WAITING,      
        OK_COMPLETED,    
        ERROR_DEAD       
    };

    // Callback now gives access to Header and Body
    // User is responsible for parsing the Header to get Type/ID/Ack
    using ClientMessageCallback = std::function<void(
        TcpSession& session,
        const std::vector<unsigned char>& header, // Raw Header
        std::vector<unsigned char>&& body         // Moved Body
    )>;

    Server(size_t header_size = 12) : HEADER_SIZE(header_size) { // Default 12 for MYMQ compatibility
        init_sys();
    }

    ~Server() {
        if (server_fd != -1) { close(server_fd); }
        if (epfd_ != -1) { close(epfd_); }
    }

    void set_client_message_callback(ClientMessageCallback cb) {
        std::unique_lock<std::shared_mutex> ulock(mtx_callback);
        client_msg_callback_ = cb;
    }




    void init_sys(){
        Config_manager cm("config/communication.propertity");
        try {
           server_IP= cm.get_size_t("IP");
        } catch (std::exception& e) {
            cerr("IP not found");
        }
        PORT= cm.getint("port");
        // HEADER_SIZE is set in constructor
        msg_body_limit=cm.getull("msgbodylimit_len");
        check_connect_liveness_ms=cm.getint("livenesscheck_ms");

    }

    void initialize(){//linux 4.18 can only use ktls 1.2
        std::cout << "[Runtime OpenSSL Version]: " << OpenSSL_version(OPENSSL_VERSION) << std::endl;
        std::cout << "[Compile-time OpenSSL Header]: " << OPENSSL_VERSION_TEXT << std::endl;
        {//KTLS
            ctx = SSL_CTX_new(TLS_server_method());
//             SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
//                        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);

            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);

            // 加密套件也配合改一下，确保兼容


            // 加载证书 (必须)
             if (SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM) <= 0 ||
                 SSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM) <= 0) {
                 cerr("Error loading cert/key") ;
                 throw std::runtime_error("Error loading cert/key");
             }

            // 【核心步骤】告诉 OpenSSL 我们想要使用 Kernel TLS

             long options = SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);

             // 检查返回值是否包含了 SSL_OP_ENABLE_KTLS 标志
             if (options & SSL_OP_ENABLE_KTLS) {
                 cerr("SSL_OP_ENABLE_KTLS option successfully set.");
             } else {
                throw std::runtime_error("Failed to open ktls :SSL_OP_ENABLE_KTLS option NOT set. Check OpenSSL version and build config.\n");

             }

             if (SSL_CTX_set_cipher_list(ctx, "AES128-GCM-SHA256") != 1) {
                 throw std::runtime_error("Error setting TLS 1.2 cipher list");
             }

//            //指定支持 kTLS 的加密套件 (AES-GCM)
//            if (SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256") != 1) {
//               throw std::runtime_error("Error setting TLS 1.3 ciphersuites");
//            }


        }

        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            perror("socket failed");
            throw std::runtime_error("Failed to create server socket.");
        }

        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
            perror("setsockopt failed");
            close(server_fd);
            throw std::runtime_error("Failed to set socket options.");
        }

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(PORT);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
            perror("bind failed");
            close(server_fd);
            throw std::runtime_error("Failed to bind server socket.");
        }

        if (listen(server_fd, 5) == -1) {
            perror("listen failed");
            close(server_fd);
            throw std::runtime_error("Failed to listen on server socket.");
        }




        if (fcntl(server_fd, F_SETFL, O_NONBLOCK) == -1) {
            perror("fcntl O_NONBLOCK for server_fd failed");
            close(server_fd);
            throw std::runtime_error("Failed to set server socket to non-blocking.");
        }

        epfd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ == -1) {
            perror("epoll_create1 failed");
            close(server_fd);
            close(epfd_);
            throw std::runtime_error("Failed to create epoll instance.");
        }

        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET; // 监听套接字也使用边缘触发模式
        event.data.fd = server_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd, &event) == -1) {
            perror("epoll_ctl (server_fd) failed");
            close(server_fd);
            close(epfd_);
            throw std::runtime_error("Failed to add server socket to epoll.");
        }

        std::vector<epoll_event> events(MAX_EVENT_NUM);


        running_ = true;

        while (running_) {
            int event_num = epoll_wait(epfd_, events.data(), MAX_EVENT_NUM, 200);

            if (event_num == -1) {
                if (errno == EINTR) {
                    cerr("[" +now_ms_time_gen_str() + "] [DEBUG] epoll_wait interrupted by signal." );
                    continue;
                }
                if (errno == EBADF && !running_.load()) {
                    cerr("[" + now_ms_time_gen_str() + "] [信息] epoll_wait returned EBADF during shutdown. Exiting loop.");
                    break;
                }
                perror("epoll_wait failed");
                if (running_) {
                    std::cerr << "[" << now_ms_time_gen_str() << "] [错误] epoll_wait failed unexpectedly, errno: " << errno << std::endl;
                }
                break;
            }


            if (!running_) {
                std::cerr << "[" << now_ms_time_gen_str() << "] [信息] running_ is false after epoll_wait, exiting loop." << std::endl;
                break;
            }


            for (int i = 0; i < event_num; i++) {
                int fd = events[i].data.fd;
                if (fd == server_fd) {
                    while (true) {
                        socklen_t addrlen = sizeof(address);
                        int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
                        if (new_socket == -1) {
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            } else {
                                perror("accept failed");
                                break;
                            }
                        }

                        if (fcntl(new_socket, F_SETFL, O_NONBLOCK) == -1) {
                            perror("fcntl O_NONBLOCK failed");
                            close(new_socket);
                            continue;
                        }

                        //ktls
                        SSL* ssl = SSL_new(ctx);
                        if (!ssl) {
                            std::cerr << "Error creating SSL structure." << std::endl;
                            close(new_socket); // 关掉 socket
                            continue;          // 跳过这个连接
                        }

                        SSL_set_fd(ssl, new_socket);

                        struct epoll_event client_event;
                        client_event.events = EPOLLIN | EPOLLET; // Removed EPOLLONESHOT for optimization
                        client_event.data.fd = new_socket;

                        // [安全检查 2] 如果加入 epoll 失败，要释放 SSL 内存
                        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, new_socket, &client_event) == -1) {
                            perror("epoll_ctl (new_socket) failed");
                            SSL_free(ssl);     // <--- 必须释放 SSL 对象，否则内存泄漏
                            close(new_socket); // 关闭 socket
                            continue;
                        }


                        ClientStateMap::accessor ac;
                        map_client_states.insert(ac,new_socket);
                        ac->second=std::make_unique<ClientState>(HEADER_SIZE);
                        ac->second->fd = new_socket; // Set FD
                        ac->second->ssl = ssl;                 // 保存 SSL 指针
                        ac->second->is_handshake_complete = false; // 标记握手未完成

                        // Set wakeup callback for async writes
                        ac->second->on_write_ready = [this](int client_fd) {
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
                            ev.data.fd = client_fd;
                            // epoll_ctl is thread-safe
                            epoll_ctl(this->epfd_, EPOLL_CTL_MOD, client_fd, &ev);
                        };

                        ac.release();

                        cerr("[" + now_ms_time_gen_str() + "] [信息] 新连接 FD: " +std::to_string( new_socket ));
                    }
                }
                else {
                                    // ============================================================
                                    //  现有连接的事件处理 (移交 Sharded ThreadPool)
                                    // ============================================================

                                    // 1. 提取必要信息
                                    int fd = events[i].data.fd;
                                    uint32_t event_flags = events[i].events;

                                    // 2. 提交任务到分片线程池
                                    // 使用 fd 作为 key，保证同一个连接永远由同一个 Worker 处理
                                    ShardedThreadPool::instance().submit(fd, [this, fd, event_flags]() {

                                        // =======================================================
                                        //  以下代码在 Worker 线程执行
                                        // =======================================================

                                        std::shared_ptr<ClientState> client = nullptr;

                                        // --- 【关键改动 1】: 极短时间的 Map 锁定 ---
                                        {
                                            ClientStateMap::const_accessor cac;
                                            // 尝试获取锁并复制 shared_ptr
                                            if (map_client_states.find(cac, fd)) {
                                                client = cac->second; // 引用计数 +1
                                            } else {
                                                // 连接可能已经被其他线程关闭或移出
                                                return;
                                            }
                                        }
                                        // --- 【关键】此时 cac 析构，Map 锁已释放！其他线程可以操作 Map ---

                                        // 再次检查原子标记，防止在获取指针后被标记为逻辑死亡
                                        if (client->is_closing.load()) {
                                            return;
                                        }

                                        bool client_alive = true;
                                        bool need_rearm_epoll = true; // 默认需要重置 EPOLLONESHOT

                                        // A. 预检错误事件
                                        if (event_flags & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                                            std::cerr << "[" << now_ms_time_gen_str() << "] [TCP] 连接断开/错误 FD: " << fd << std::endl;
                                            client_alive = false;
                                        }

                                        // B. 核心逻辑 (握手 或 读数据)
                                        // 注意：后续所有 client-> 操作都是安全的，因为我们持有 shared_ptr
                                        if (client_alive && (event_flags & EPOLLIN)) {

                                            // --- [1. 握手逻辑: 只有未完成时才进入] ---
                                            if (!client->is_handshake_complete) {
                                                int ret = SSL_accept(client->ssl);

                                                if (ret == 1) {
                                                    // === 握手成功 ===
                                                    client->is_handshake_complete = true;
                                                    if (BIO_get_ktls_send(SSL_get_wbio(client->ssl))) {
                                                        client->enable_sendfile = true;
                                                        std::cout << "[" << now_ms_time_gen_str() << "] [kTLS] Enabled FD: " << fd << std::endl;
                                                    }
                                                    // 握手刚成功，继续尝试读取业务数据
                                                }
                                                else {
                                                    // === 握手未完成或失败 ===
                                                    int err = SSL_get_error(client->ssl, ret);

                                                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                                                        // 需要更多数据，退出并等待下次事件
                                                        goto CHECK_REARM;
                                                    }
                                                    else {
                                                        // 致命错误
                                                        std::cerr << "SSL Handshake Error FD: " << fd << ", Code: " << err << std::endl;
                                                        client_alive = false;
                                                    }
                                                }
                                            }

                                            // --- [2. 业务数据处理: 只有握手完成了才做] ---
                                            if (client_alive && client->is_handshake_complete) {
                                                // 调用 handle_client
                                                // 【注意】handle_client 内部应适配 shared_ptr 或引用，
                                                // 如果它需要调用 handle_event，请确保传入 shared_ptr
                                                IOStatus status = handle_client(fd, client); // 建议重载 handle_client 接受 shared_ptr

                                                if (status == IOStatus::ERROR_DEAD) {
                                                    client_alive = false;
                                                }
                                                
                                                // [性能优化] 处理完业务逻辑后，如果有数据待发送，尝试立即发送
                                                // 避免依赖下一次 epoll_wait 的 EPOLLOUT 触发，减少系统调用和上下文切换
                                                if (client_alive && !client->send_queue.empty()) {
                                                     // 直接复用 handle_write_event 逻辑
                                                     client_alive = handle_write_event(fd, *client);
                                                }
                                            }
                                        }

                                        // C. 处理写事件 (如果有)
                                        if (client_alive && (event_flags & EPOLLOUT)) {
                                            // handle_write_event 内部不需要再查 Map，直接用 client 操作
                                            client_alive = handle_write_event(fd, *client);
                                        }

                                        // --- 标签: 检查是否需要更新 EPOLL 状态 ---
                                        CHECK_REARM:

                                        if (client_alive) {
                                            bool should_be_writing = false;
                                            {
                                                std::lock_guard<std::mutex> lock(client->send_queue_mtx);
                                                should_be_writing = client->is_writing;
                                            }

                                            // Optimization: Only call epoll_ctl if the state actually changes
                                            if (should_be_writing && !client->epoll_out_registered) {
                                                // Need to ADD EPOLLOUT
                                                struct epoll_event ev;
                                                ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
                                                ev.data.fd = fd;
                                                if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
                                                    client->epoll_out_registered = true;
                                                } else {
                                                    // Handle error (e.g. client disconnected)
                                                    client_alive = false;
                                                }
                                            } 
                                            else if (!should_be_writing && client->epoll_out_registered) {
                                                // Need to REMOVE EPOLLOUT
                                                struct epoll_event ev;
                                                ev.events = EPOLLIN | EPOLLET;
                                                ev.data.fd = fd;
                                                if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
                                                    client->epoll_out_registered = false;
                                                } else {
                                                     client_alive = false;
                                                }
                                            }
                                            // Else: No change needed, save the syscall!
                                        }

                                        // =======================================================
                                        //  E. 连接销毁/清理逻辑 (重新获取锁移出 Map, Close FD)
                                        // =======================================================
                                        if (!client_alive) {
                                            // 1. 标记逻辑死亡，阻止后续异步操作
                                            client->is_closing.store(true);

                                            // 2. 从 Map 中移除 (需要重新获取锁)
                                            {
                                                ClientStateMap::accessor ac;
                                                if (map_client_states.find(ac, fd)) {
                                                    map_client_states.erase(ac);
                                                }
                                            }

                                            // 3. 关闭连接
                                            // fd 是值拷贝进来的，依然有效
                                            // client 指针在这里析构时，如果引用计数归零，会自动释放 ClientState 内存
                                            close_connection(fd);
                                        }
                                    });


                }
            }

        }


        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
        if (epfd_ != -1) {
            close(epfd_);
            epfd_ = -1;
        }

    }

    void close_connection(int fd){
        auto time = now_ms_time_gen_str();
        std::string clientid_to_log = "UNKNOWN";

        ClientStateMap::accessor ac;
        if(map_client_states.find(ac,fd)){
            clientid_to_log = ac->second->clientid;
            map_client_states.erase(ac);
        }
        else{
            std::cerr << "[" << now_ms_time_gen_str() << "] [Error] FD '" << fd << "' NOT FOUND " << std::endl;
        }
        ac.release();

        out("[" + time + "][ClientID: " + clientid_to_log + "][State: Offline]" );

        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    bool process_send_queue(SSL* ssl, ClientState& state, std::unique_lock<std::mutex>& ulock_send) {
        if (!ulock_send.owns_lock()) {

                ulock_send.lock();
            }
        // Loop while there are items in the queue
        while (!state.send_queue.empty()) {
            ClientState::SendItem& current_item = state.send_queue.front();

            bool item_sent_completely = false;
            bool error_occurred = false;
            bool should_break_and_wait = false; // Flag for EAGAIN/EWOULDBLOCK

            // std::visit logic is copied directly from your original code.


            // std::visit logic
                        std::visit([&](auto&& arg) {
                            using T = std::decay_t<decltype(arg)>;

                            // --- 1. Handle sending a regular byte message ---
                            if constexpr (std::is_same_v<T, std::vector<unsigned char>>) {
                                std::vector<unsigned char>& message = arg;
                                const char* buffer_ptr = reinterpret_cast<const char*>(message.data() + state.current_vec_send_offset);
                                size_t remaining_length = message.size() - state.current_vec_send_offset;
                                size_t written_bytes = 0;

                                // [Change 1] 使用 SSL_write_ex
                                int ret = SSL_write_ex(ssl, buffer_ptr, remaining_length, &written_bytes);

                                if (ret == 1) { // Success
                                    state.current_vec_send_offset += written_bytes;
                                    if (state.current_vec_send_offset == message.size()) {
                                        item_sent_completely = true;
                                        state.current_vec_send_offset = 0; // Reset for next usage if needed
                                    }
                                    // 如果没发完，item_sent_completely 为 false，下次循环继续
                                } else { // Failure (ret == 0)
                                    // [Change 2] 获取 OpenSSL 错误码，而不是直接看 errno
                                    int err = SSL_get_error(ssl, 0);

                                    switch (err) {
                                        case SSL_ERROR_WANT_WRITE:
                                        case SSL_ERROR_WANT_READ:
                                            // 内核缓冲区满，或 SSL 握手/重协商需要读数据
                                            should_break_and_wait = true;
                                            return;

                                        case SSL_ERROR_SYSCALL:
                                            if (errno == EINTR) return; // 被信号中断，重试
                                            // 真正的网络错误
                                            perror("SSL_write_ex syscall failed");
                                            error_occurred = true;
                                            return;

                                        default:
                                            // 协议层错误 (SSL_ERROR_SSL 等)
                                            std::cerr << "SSL_write_ex error: " << err << std::endl;
                                            error_occurred = true;
                                            return;
                                    }
                                }

                            // --- 2. Handle sending a SendFileTask (header + file) ---
                            } else if constexpr (std::is_same_v<T, FileSendTask>) {
                               FileSendTask& task = arg;

                                // --- 2a. Send the header first ---
                                if (task.header_send_offset < task.header_data.size()) {
                                    
                                    const char* buffer_ptr = reinterpret_cast<const char*>(task.header_data.data() + task.header_send_offset);
                                    size_t remaining_header_length = task.header_data.size() - task.header_send_offset;
                                    size_t written_bytes = 0;

                                    // [Change 3] Header 发送也改用 SSL_write_ex
                                    int ret = SSL_write_ex(ssl, buffer_ptr, remaining_header_length, &written_bytes);

                                    if (ret == 1) {
                                        task.header_send_offset += written_bytes;
                                        if (task.header_send_offset < task.header_data.size()) {
                                            should_break_and_wait = true; // 没发完通常意味着 buffer 满
                                            return;
                                        }
                                        // Header sent completely, proceed to file
                                    } else {
                                        int err = SSL_get_error(ssl, 0);
                                        switch (err) {
                                            case SSL_ERROR_WANT_WRITE:
                                            case SSL_ERROR_WANT_READ:
                                                should_break_and_wait = true;
                                                return;
                                            case SSL_ERROR_SYSCALL:
                                                if (errno == EINTR) return;
                                                perror("send header failed");
                                                error_occurred = true;
                                                return;
                                            default:
                                                std::cerr << "SSL error sending header: " << err << std::endl;
                                                error_occurred = true;
                                                return;
                                        }
                                    }
                                } 

                                // --- 2b. Send the file data (kTLS Zero-copy) ---
                                if (task.header_send_offset == task.header_data.size()) {

                                    off_t current_file_offset = task.offset + task.sent_so_far;
                                    size_t remaining_file_length = task.length - task.sent_so_far;

                                    if (remaining_file_length == 0) {
                                        item_sent_completely = true;
                                        return;
                                    }

                                    if (!BIO_get_ktls_send(SSL_get_wbio(ssl))) {
                                        std::cerr << "kTLS not enabled, cannot use sendfile" << std::endl;
                                                                 error_occurred = true;
                                                                 return;

                                    }
                                    // [Change 4] 使用 SSL_sendfile
                                    // 1. offset 传值 (current_file_offset)，不是指针
                                    // 2. flags 填 0
                                    ossl_ssize_t bytes_sent_file = SSL_sendfile(ssl, task.in_fd, current_file_offset, remaining_file_length, 0);

                                    if (bytes_sent_file > 0) {
                                        task.sent_so_far += bytes_sent_file;
                                        if (task.sent_so_far == task.length) {
                                            item_sent_completely = true;
                                        }
                                        // 如果一次没发完（kTLS 分片限制或 buffer 限制），下次循环继续
                                    } else {
                                        // [Change 5] 处理 SSL_sendfile 的特定错误
                                        int err = SSL_get_error(ssl, bytes_sent_file);

                                        switch (err) {
                                            case SSL_ERROR_NONE:
                                                // 理论上 bytes_sent_file > 0 才会是 NONE，但为了稳健
                                                break;

                                            case SSL_ERROR_WANT_WRITE:
                                            case SSL_ERROR_WANT_READ:
                                            case SSL_ERROR_WANT_ASYNC: // kTLS 可能会用到
                                                should_break_and_wait = true;
                                                return;

                                            case SSL_ERROR_SYSCALL:
                                                if (errno == EINTR) return;
                                                // 某些情况下 sendfile 底层返回 EAGAIN 会被转为 SYSCALL + errno
                                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                    should_break_and_wait = true;
                                                    return;
                                                }
                                                perror("SSL_sendfile failed");
                                                error_occurred = true;
                                                return;

                                            default:
                                                std::cerr << "SSL_sendfile error: " << err << std::endl;
                                                error_occurred = true;
                                                return;
                                        }
                                    }
                                } // end if (task.header_sent)
                            } // end of SendFileTask variant
                        }, current_item);



            if (error_occurred) {
                // Unrecoverable error. Clear queue, unregister, return false.
                state.send_queue.clear();
                state.is_writing = false;

//          update_epoll_events(SSL_get_fd(ssl), EPOLLIN | EPOLLET);

                return false; // Signal to close connection
            }

            if (should_break_and_wait) {

                return true;
            }

            if (item_sent_completely) {
                // This item is done. Remove it and loop to try the next one.
                state.send_queue.pop_front();
            }

        } // end while (!state.send_queue.empty())

        // --- Queue is now empty ---
        // If we finished the loop, the queue is empty.
        // We can unregister for EPOLLOUT.
        state.is_writing = false;

//update_epoll_events(SSL_get_fd(ssl), EPOLLIN | EPOLLET);

        return true; // Connection is alive
    }
    // 处理 EPOLLOUT 事件
    bool handle_write_event(int sock, ClientState& state) {
        // 这里的锁是必须的，保护队列
        std::unique_lock<std::mutex> ulock_send(state.send_queue_mtx);

        // 4. 检查队列是否为空
        if (state.send_queue.empty()) {
            // 队列空了，说明没东西写了
            if (state.is_writing) {
                state.is_writing = false;

            }
            return true;
        }

        bool is_alive = process_send_queue(state.ssl, state, ulock_send);

        return is_alive;
    }

    void stop() {
        if (!running_.load()) { // 如果已经停止，直接返回
            return;
        }
        running_ = false; // 设置停止标志
        std::cerr << "[" << now_ms_time_gen_str() << "] [信息] Server::stop() called. Setting running_ to false." << std::endl;

        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
            std::cerr << "[" << now_ms_time_gen_str() << "] [信息] Server socket (FD: " << server_fd << ") explicitly closed in stop()." << std::endl;
        }

    }

    void start(){
        running_=true;
    }

    std::thread start_in_thread() {
        running_ = true; // 确保在线程启动前设置运行标志
        return std::thread([this]() {
            this->initialize();
        });
    }

private:




    void update_epoll_events(int fd, uint32_t events) {
        struct epoll_event event;
        event.events = events;
        event.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
            perror("epoll_ctl_mod failed");
            std::cerr << "[" << now_ms_time_gen_str() << "] [错误] epoll_ctl_mod for FD: " << fd << " with events: " << events << " failed: " << strerror(errno) << std::endl;
            // 实际应用中可能需要更复杂的错误处理，例如关闭连接
        }
    }

    void handle_event(std::shared_ptr<ClientState> state, std::vector<unsigned char>&& body) {
        std::shared_lock<std::shared_mutex> ulock(mtx_callback);
        if (client_msg_callback_) {
            TcpSession session(state);
            client_msg_callback_(session, state->header_buffer, std::move(body));
        }
    }

    void process_message( int sock, Mybyte&& body,std::shared_ptr<ClientState> state) {
        handle_event(state, std::move(body));
    }

    IOStatus handle_client(int sock, std::shared_ptr<ClientState> state) {

        // --- 状态 1: 正在读取头部 ---
        if (state->bytes_read_in_header < HEADER_SIZE) {

            size_t bytes_read_this_time = 0;
            int ret = SSL_read_ex(state->ssl,
                                  reinterpret_cast<char*>(state->header_buffer.data() + state->bytes_read_in_header),
                                  HEADER_SIZE - state->bytes_read_in_header,
                                  &bytes_read_this_time);

            if (ret == 1) { // 【成功读取】
                state->bytes_read_in_header += bytes_read_this_time;

                if (state->bytes_read_in_header == HEADER_SIZE) {
                    // --- 解析消息头 ---
                    uint32_t total_length_net;
                    memcpy(&total_length_net, state->header_buffer.data(), sizeof(uint32_t));
                    uint32_t total_length = ntohl(total_length_net);

                    uint16_t event_type_net;
                    memcpy(&event_type_net, state->header_buffer.data() + sizeof(uint32_t), sizeof(uint16_t));
                    state->event_type = ntohs(event_type_net);

                    uint32_t correlation_id_net;
                    memcpy(&correlation_id_net, state->header_buffer.data() + sizeof(uint32_t) + sizeof(uint16_t), sizeof(uint32_t));
                    state->correlation_id = ntohl(correlation_id_net);

                    uint16_t ack_level_net;
                    memcpy(&ack_level_net, state->header_buffer.data() + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t), sizeof(uint16_t));
                    state->ack_level = ntohs(ack_level_net);

                    // --- 验证消息头 ---
                    if (total_length < HEADER_SIZE) {
                        std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到畸形消息 (FD: " << sock << "): total_length (" << total_length << ") < HEADER_SIZE (" << HEADER_SIZE << ")" << std::endl;
                        return IOStatus::ERROR_DEAD;
                    }
                    state->expected_body_length = total_length - HEADER_SIZE;

//                    if (state->expected_body_length > msg_body_limit) {
//                        std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到过大消息体 (FD: " << sock << "): " << state->expected_body_length << " bytes, limit is " << msg_body_limit << std::endl;
//                        return IOStatus::ERROR_DEAD;
//                    }

                    if (state->expected_body_length == 0) {
                        state->current_state = ClientState::READING_HEADER;
                        state->bytes_read_in_header = 0;
                        state->bytes_read_in_body = 0;

                        // 【改动 2】process_message 需要接收 shared_ptr state
                        // 这样 handle_event 才能把 session 传给用户
                        process_message(sock, Mybyte{}, state);

                        return IOStatus::OK_COMPLETED;
                    } else {
                        // 有消息体，准备读取
                        state->body_buffer.clear();
                        state->body_buffer.resize(state->expected_body_length);
                        state->bytes_read_in_body = 0;
                        state->current_state = ClientState::READING_BODY;
                        // fall-through
                    }
                }
            } else { // ret == 0
                int err = SSL_get_error(state->ssl, 0);
                switch (err) {
                    case SSL_ERROR_WANT_READ:
                    case SSL_ERROR_WANT_WRITE:
                        return IOStatus::OK_WAITING;
                    case SSL_ERROR_ZERO_RETURN:
                        std::cerr << "[" << now_ms_time_gen_str() << "] [信息] 客户端断开 (TLS Close Notify)。" << std::endl;
                        return IOStatus::ERROR_DEAD;
                    case SSL_ERROR_SYSCALL:
                        if (errno == EINTR) return IOStatus::OK_WAITING;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) return IOStatus::OK_WAITING;
                        // perror("read header syscall failed"); // 可选日志
                        return IOStatus::ERROR_DEAD;
                    default:
                        std::cerr << "[" << now_ms_time_gen_str() << "] [错误] SSL_read_ex header error: " << err << std::endl;
                        return IOStatus::ERROR_DEAD;
                }
            }
        }

        // --- 状态 2: 正在读取消息体 ---
        if (state->current_state == ClientState::READING_BODY) {
            // 确保 buffer 大小
            if (state->body_buffer.size() < state->expected_body_length) {
                state->body_buffer.resize(state->expected_body_length);
            }

            if (state->bytes_read_in_body < state->expected_body_length) {
                size_t bytes_read_this_time = 0;
                int ret = SSL_read_ex(state->ssl,
                                      reinterpret_cast<char*>(state->body_buffer.data() + state->bytes_read_in_body),
                                      state->expected_body_length - state->bytes_read_in_body,
                                      &bytes_read_this_time);

                if (ret == 1) {
                    state->bytes_read_in_body += bytes_read_this_time;

                    if (state->bytes_read_in_body == state->expected_body_length) {


                        state->current_state = ClientState::READING_HEADER;
                        state->bytes_read_in_header = 0;
                        state->bytes_read_in_body = 0;
                        state->expected_body_length = 0;
                        process_message(sock, std::move(state->body_buffer), state);

                        return IOStatus::OK_COMPLETED;
                    }
                    return IOStatus::OK_WAITING;
                } else {
                    int err = SSL_get_error(state->ssl, 0);
                    switch (err) {
                        case SSL_ERROR_WANT_READ:
                        case SSL_ERROR_WANT_WRITE:
                            return IOStatus::OK_WAITING;
                        case SSL_ERROR_ZERO_RETURN:
                            std::cerr << "[" << now_ms_time_gen_str() << "] [信息] 客户端断开 (TLS Close Body)。" << std::endl;
                            return IOStatus::ERROR_DEAD;
                        case SSL_ERROR_SYSCALL:
                            if (errno == EINTR) return IOStatus::OK_WAITING;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) return IOStatus::OK_WAITING;
                            return IOStatus::ERROR_DEAD;
                        default:
                            std::cerr << "[" << now_ms_time_gen_str() << "] [错误] SSL_read_ex body error: " << err << std::endl;
                            return IOStatus::ERROR_DEAD;
                    }
                }
            }
        }

        return IOStatus::OK_WAITING;
    }

    void handle_event(std::shared_ptr<ClientState> state, Mybyte msg_body) {

        // 1. 准备回调
        ClientMessageCallback curr_cb;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_callback);
            curr_cb = client_msg_callback_;
        }

        if (curr_cb) {
            TcpSession session(state);
            short type = state->event_type;
            uint32_t cid = state->correlation_id;
            uint16_t ack = state->ack_level;
            curr_cb(session, type, cid, ack, std::move(msg_body));
        }
    }

private:
    int epfd_;
    const int MAX_EVENT_NUM = 1000;
    std::atomic<bool> running_{0};
    int server_fd;
    struct sockaddr_in address;
    std::string server_IP;


    tbb::concurrent_hash_map<int,  std::shared_ptr<ClientState>> map_client_states;

     tbb::concurrent_hash_map<int, std::string> map_clientid;
    ClientMessageCallback client_msg_callback_;
    std::shared_mutex mtx_callback; // 保护 client_msg_callback_

    size_t msg_body_limit;
    int PORT;
    size_t HEADER_SIZE;
    int check_connect_liveness_ms;



    SSL_CTX* ctx=nullptr;

    ShardedThreadPool& pool = ShardedThreadPool::instance(8);


};

#endif
