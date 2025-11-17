#ifndef SERVER_H
#define SERVER_H

#include "Timer.h"
#include "CONFIG_MANAGER.h"
#include "MYMQ_innercodes.h"
#include"MYMQ_Publiccodes.h"
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

using Mybyte = std::vector<unsigned char>;
using Eve= MYMQ::EventType;
using MesLoc=MYMQ::MYMQ_Server::MessageLocation;
using Err=MYMQ_Public::CommonErrorCode;
// 辅助函数（保持不变）


void out(const std::string& str, bool perior = 0){
    Printqueue::instance().out(str,0,perior);
}

void cerr(const std::string& str, bool perior = 0){
    Printqueue::instance().out(str,1,perior);
}


std::string now_ms_time_gen_str() {
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    std::tm p_tm_storage; // 使用本地 tm 结构体
    std::tm* p_tm = nullptr;

#ifdef _WIN32
    // 在 Windows 上使用 localtime_s
    if (localtime_s(&p_tm_storage, &now_c) == 0) {
        p_tm = &p_tm_storage;
    }
#else
    // 在 POSIX 系统上使用 localtime_r
    if (localtime_r(&now_c, &p_tm_storage) != nullptr) {
        p_tm = &p_tm_storage;
    }
#endif

    if (p_tm == nullptr) {
        // 处理错误，例如返回一个默认字符串
        return "[Time Error]";
    }

    std::stringstream ss_full;
    ss_full << std::put_time(p_tm, "%Y-%m-%d %H:%M:%S");

    auto duration_since_epoch = now.time_since_epoch();
    auto seconds_part = std::chrono::duration_cast<std::chrono::seconds>(duration_since_epoch);
    auto fractional_seconds = duration_since_epoch - seconds_part;
    auto milliseconds_part = std::chrono::duration_cast<std::chrono::milliseconds>(fractional_seconds);

    ss_full << "." << std::setfill('0') << std::setw(3) << milliseconds_part.count();
    return ss_full.str();
}

class Server{
public:
    // ClientState 结构体用于管理每个客户端的读取状态

    struct ClientState {
            enum State {
                READING_HEADER,
                READING_BODY

            };
        ////////////////
            State current_state = READING_HEADER;


            std::vector<unsigned char> header_buffer;
            size_t bytes_read_in_header = 0;

            std::vector<unsigned char> body_buffer;
            size_t bytes_read_in_body = 0;

            uint32_t expected_body_length = 0;
            uint16_t event_type = 0;

            uint32_t correlation_id = 0;
            uint16_t ack_level = 0;
      ///////////////
            std::mutex mtx;


            bool id_registered = false;
            std::string clientid="UNKNOWN";



            struct SendFileTask {
                int in_fd;             // 源文件的文件描述符
                off_t offset;          // 文件中读取的初始偏移量 (MesLoc.offset_in_file)
                size_t total_length;   // 此任务要发送的总长度 (MesLoc.length)
                size_t sent_so_far;    // 此任务已发送的字节数
                bool header_sent;
                size_t offset_batch_first;
                std::string topicname;
                size_t partition;
                uint32_t correlation_id;
                uint16_t ack_level;

                std::vector<unsigned char> header_data; // 新增：存储构建好的头部
                size_t header_send_offset; // 新增：头部已发送的偏移量
                SendFileTask(int fd, off_t off, size_t len, size_t first_off, const std::string& topic, size_t par, uint32_t correlation_id, uint16_t ack_level)
                    : in_fd(fd), offset(off), total_length(len), sent_so_far(0), header_sent(0), header_send_offset(0), offset_batch_first(first_off),
                    correlation_id(correlation_id), ack_level(ack_level)
                    , topicname(topic), partition(par) {}
            };


            // 统一队列，用于常规消息 (std::vector<unsigned char>) 和文件发送 (SendFileTask)
            using SendItem = std::variant<std::vector<unsigned char>, SendFileTask>;
            std::deque<SendItem> send_queue;

            // ... (current_vec_send_offset, is_writing, send_queue_mtx 保持不变) ...
            size_t current_vec_send_offset = 0;
            bool is_writing = false;
            std::mutex send_queue_mtx;

            // 构造函数：初始化 header_buffer 的大小
            ClientState(size_t header_size)
                : header_buffer(header_size)
            {
                // C++11 的类内成员初始化会负责设置
                // current_state = READING_HEADER 和 id_registered = false
            }

            ClientState() = default;
        };

    using ClientStateMap=tbb::concurrent_hash_map<int, std::unique_ptr<ClientState>>;

    Server() {
        init_sys();
    }

    ~Server() {

        if (server_fd != -1) {
            close(server_fd);
            cerr( "Server socket closed." );

        }
        if (epfd_ != -1) {
            close(epfd_);
            cerr("Epoll instance closed.");

        }
    }

    using ClientMessageCallback = std::function<void(int client_fd,std::string clientid, short event_type,uint32_t correlation_id,uint16_t ack_level,const Mybyte& msg_body)>;

    // 设置回调函数的方法
    void set_client_message_callback(ClientMessageCallback cb) {
        std::unique_lock<std::shared_mutex> ulock(mtx_callback);
        client_msg_callback_ = cb;
    }



    void init_sys(){
        Config_manager cm("config/communication.propertity");
        PORT= cm.getint("port");
        HEADER_SIZE=MYMQ::HEADER_SIZE;
        msg_body_limit=cm.getull("msgbodylimit_len");
        check_connect_liveness_ms=cm.getint("livenesscheck_ms");
    }

    void initialize(){

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
            int event_num = epoll_wait(epfd_, events.data(), MAX_EVENT_NUM, -1);

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

                        struct epoll_event client_event;
                        client_event.events = EPOLLIN | EPOLLET;
                        client_event.data.fd = new_socket;
                        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, new_socket, &client_event) == -1) {
                            perror("epoll_ctl (new_socket) failed");
                            close(new_socket);
                            continue;
                        }

                        ClientStateMap::accessor ac;
                        map_client_states.insert(ac,new_socket);
                        ac->second=std::make_unique<ClientState>(HEADER_SIZE);
                        cerr("[" + now_ms_time_gen_str() + "] [信息] 新连接 FD: " +std::to_string( new_socket ));
                    }
                }
                else {

                    bool client_alive = true;
                    if (events[i].events & EPOLLIN) {
                        client_alive = handle_client(fd);
                    }

                    if (client_alive && (events[i].events & EPOLLOUT)) {
                        client_alive = handle_write_event(fd);
                    }

                    if (!client_alive) {
                        auto time = now_ms_time_gen_str();
                        std::string clientid_to_log = "UNKNOWN";



                        ClientStateMap::accessor ac;
                        if(map_client_states.find(ac,fd)){
                            map_client_states.erase(fd);
                        }
                        else{
                            std::cerr << "[" << now_ms_time_gen_str() << "] [Error] FD '" << fd << "' NOT FOUND " << std::endl;
                        }
                        tbb::concurrent_hash_map<int, std::string>::accessor ac1;
                        if(map_clientid.find(ac1,fd)){
                            clientid_to_log=ac1->second;
                            map_clientid.erase(fd);
                        }
                        else{
                              std::cerr << "[" << now_ms_time_gen_str() << "] [Error] FD '" << fd<< "' NOT FOUND " << std::endl;
                        }

                        out("[" + time + "][用户: " + clientid_to_log + "][状态：刚刚离线]" );

                        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                        shutdown(fd, SHUT_RDWR);
                        close(fd);
                    }
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


    Err pull_out(int target_fd, uint32_t correlation_id, uint16_t ack_level, MesLoc locinf, const std::string& topicname, int partition) {
        if (!locinf.found || locinf.file_descriptor == -1) {
            return Err::NO_RECORD;
        }

        // Create the task
        ClientState::SendFileTask file_task(locinf.file_descriptor,
                                            locinf.offset_in_file,
                                            locinf.length,
                                            locinf.offset_batch_first,
                                            topicname,
                                            partition,
                                            correlation_id,
                                            ack_level);

        // Enqueue the task (as a variant)
        return enqueue_send_item(target_fd, std::move(file_task));
    }


    Err send_msg(int target_fd, Eve event_type, uint32_t correlation_id, uint16_t ack_level, const Mybyte& msg_body) {
        // Message building logic is unchanged
        short event_type_ = static_cast<short>(event_type);
        MessageBuilder mb;
        uint32_t total_length = static_cast<uint32_t>(HEADER_SIZE + sizeof(uint32_t) + msg_body.size());
        mb.reserve(total_length);
        mb.append_uint32(total_length);
        mb.append_uint16(event_type_);
        mb.append_uint32(correlation_id);
        mb.append_uint16(static_cast<uint16_t>(ack_level));
        mb.append_uchar_vector(msg_body);
        Mybyte full_message = std::move(mb.data);


        return enqueue_send_item(target_fd, std::move(full_message));
    }


    Err enqueue_send_item(int sock, ClientState::SendItem item_to_send) {

        ClientStateMap::const_accessor ac;
        if (!map_client_states.find(ac, sock)) {
            std::cerr << "[" << now_ms_time_gen_str() << "] [Error] Client " << sock
                      << " NOT FOUND in enqueue_send_item" << std::endl;
            return Err::CLIENT_LINK_NOT_FOUND;
        }


        auto& state_ptr = ac->second;

        bool needs_wakeup = false;
        {
            std::unique_lock<std::mutex> statelock(state_ptr->send_queue_mtx);

            // 3. Add the work item to the queue.
            state_ptr->send_queue.push_back(std::move(item_to_send));

            if (!state_ptr->is_writing) {
                state_ptr->is_writing = true; // Mark as "work pending"
                needs_wakeup = true;
            }
        } // 5. Mutex is released here.

        if (needs_wakeup) {
            update_epoll_events(sock, EPOLLIN | EPOLLOUT | EPOLLET);
        }

        return Err::NULL_ERROR;
    }


    bool process_send_queue(int sock, ClientState& state, std::unique_lock<std::mutex>& ulock_send) {

        // Loop while there are items in the queue
        while (!state.send_queue.empty()) {
            ClientState::SendItem& current_item = state.send_queue.front();

            bool item_sent_completely = false;
            bool error_occurred = false;
            bool should_break_and_wait = false; // Flag for EAGAIN/EWOULDBLOCK

            // std::visit logic is copied directly from your original code.
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;

                // --- 1. Handle sending a regular byte message ---
                if constexpr (std::is_same_v<T, std::vector<unsigned char>>) {
                    std::vector<unsigned char>& message = arg;
                    const char* buffer_ptr = reinterpret_cast<const char*>(message.data() + state.current_vec_send_offset);
                    size_t remaining_length = message.size() - state.current_vec_send_offset;

                    ssize_t bytes_sent = send(sock, buffer_ptr, remaining_length, 0);

                    if (bytes_sent < 0) {
                        if (errno == EINTR) { return; } // Interrupted, just retry
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            should_break_and_wait = true; // Buffer full
                            return;
                        }
                        perror("send failed in process_send_queue for regular message");
                        error_occurred = true;
                        return;
                    } else if (bytes_sent == 0) {
                        std::cerr << "[" << now_ms_time_gen_str() << "][FD: " << sock << "] : send returned 0 bytes." << std::endl;
                        error_occurred = true;
                        return;
                    } else {
                        state.current_vec_send_offset += bytes_sent;
                        if (state.current_vec_send_offset == message.size()) {
                            item_sent_completely = true;
                            state.current_vec_send_offset = 0; // Reset for next vector
                        }
                        // If partial send, item_sent_completely remains false
                        // and we will loop again on the same item.
                    }

                // --- 2. Handle sending a SendFileTask (header + file) ---
                } else if constexpr (std::is_same_v<T, ClientState::SendFileTask>) {
                    ClientState::SendFileTask& task = arg;

                    // --- 2a. Send the header first ---
                    if (!task.header_sent) {
                        if (task.header_data.empty()) {


                            MessageBuilder mb_pull_inf_additional;

                            mb_pull_inf_additional.append_short(static_cast<short>(Err::NULL_ERROR));
                            mb_pull_inf_additional.append_size_t(task.offset_batch_first);
                            mb_pull_inf_additional.append_string(task.topicname);
                            mb_pull_inf_additional.append_size_t(task.partition);
                            auto pull_inf_additional=std::move(mb_pull_inf_additional.data) ;

                            MessageBuilder mb;
                            mb.reserve(HEADER_SIZE +sizeof(short) +sizeof (size_t)+sizeof(uint32_t));
                            uint32_t total_message_length =
                                    static_cast<uint32_t>(HEADER_SIZE +pull_inf_additional.size()+sizeof(size_t)+ task.total_length);
                            mb.append_uint32(total_message_length);
                            mb.append_uint16(static_cast<uint16_t>(Eve::SERVER_RESPONSE_PULL_DATA));
                            mb.append(task.correlation_id,task.ack_level);//6
                            mb.append(pull_inf_additional);
                            mb.append_uint32(static_cast<uint32_t>(task.total_length));
                            task.header_data = std::move(mb.data);
                            task.header_send_offset = 0;
                        }

                        const char* buffer_ptr = reinterpret_cast<const char*>(task.header_data.data() + task.header_send_offset);
                        size_t remaining_header_length = task.header_data.size() - task.header_send_offset;
                        ssize_t bytes_sent_header = send(sock, buffer_ptr, remaining_header_length, 0);

                        if (bytes_sent_header < 0) {
                            if (errno == EINTR) { return; }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                should_break_and_wait = true;
                                return; // Wait for buffer
                            }
                            perror("send header failed");
                            error_occurred = true;
                            return;
                        } else if (bytes_sent_header == 0) {
                            std::cerr << "[" << now_ms_time_gen_str() << "][FD: " << sock << "] : send header returned 0 bytes." << std::endl;
                            error_occurred = true;
                            return;
                        } else {
                            task.header_send_offset += bytes_sent_header;
                            if (task.header_send_offset == task.header_data.size()) {
                                task.header_sent = true; // Header is fully sent
                            } else {
                                // Header partially sent, need to wait
                                should_break_and_wait = true;
                                return;
                            }
                        }
                    } // end if (!task.header_sent)

                    // --- 2b. Send the file data (if header is sent) ---
                    if (task.header_sent) {
                        off_t current_file_offset_for_sendfile = task.offset + task.sent_so_far;
                        size_t remaining_file_length = task.total_length - task.sent_so_far;

                        if (remaining_file_length == 0) {
                            item_sent_completely = true; // Nothing left to send
                            return;
                        }

                        // NOTE: Your original code's sendfile call is missing on some OSes.
                        // This assumes a Linux-like sendfile.
                        // If on Windows, you'd use TransmitFile.
                        // Make sure this matches your OS.
                        ssize_t bytes_sent_file = sendfile(sock, task.in_fd, &current_file_offset_for_sendfile, remaining_file_length);

                        if (bytes_sent_file < 0) {
                            if (errno == EINTR) { return; } // Retry
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                should_break_and_wait = true; // Wait for buffer
                                return;
                            }
                            perror("sendfile failed");
                            error_occurred = true;
                            return;
                        } else if (bytes_sent_file == 0) {
                            // This might happen if the file handle is weird,
                            // but usually sendfile handles this.
                            std::cerr << "[" << now_ms_time_gen_str() << "][FD: " << sock << "] : sendfile returned 0 bytes." << std::endl;
                            error_occurred = true;
                            return;
                        } else {
                            task.sent_so_far += bytes_sent_file;
                            if (task.sent_so_far == task.total_length) {
                                item_sent_completely = true; // File is complete
                            }
                            // If partial send, item_sent_completely remains false
                        }
                    } // end if (task.header_sent)
                } // end of SendFileTask variant
            }, current_item);

            // --- Handle loop/state changes based on visit results ---

            if (error_occurred) {
                // Unrecoverable error. Clear queue, unregister, return false.
                state.send_queue.clear();
                state.is_writing = false;

                ulock_send.unlock(); // Release lock to update epoll
                update_epoll_events(sock, EPOLLIN | EPOLLET); // Remove EPOLLOUT
                ulock_send.lock();   // Re-acquire lock

                return false; // Signal to close connection
            }

            if (should_break_and_wait) {
                // Send buffer is full. We must stop processing and wait.
                // state.is_writing is still true.
                // EPOLLOUT is still registered.
                // Just return. We'll be called again later.
                return true; // Connection is alive, but waiting
            }

            if (item_sent_completely) {
                // This item is done. Remove it and loop to try the next one.
                state.send_queue.pop_front();
            } else {
                // Item was not fully sent (partial send), but no error
                // and not EWOULDBLOCK. Loop will retry the *same item*
                // with updated offsets (e.g., current_vec_send_offset).
            }

        } // end while (!state.send_queue.empty())

        // --- Queue is now empty ---
        // If we finished the loop, the queue is empty.
        // We can unregister for EPOLLOUT.
        state.is_writing = false;

        ulock_send.unlock(); // Release lock to update epoll
        update_epoll_events(sock, EPOLLIN | EPOLLET); // Remove EPOLLOUT
        ulock_send.lock();   // Re-acquire lock

        return true; // Connection is alive
    }
    // 处理 EPOLLOUT 事件
    bool handle_write_event(int sock) {

        ClientStateMap::const_accessor ac;
        if (!map_client_states.find(ac, sock)) {
            std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到未知FD的EPOLLOUT事件: "
                      << sock << std::endl;
            return false;
        }

        ClientState& state = *(ac->second);

        std::unique_lock<std::mutex> ulock_send(state.send_queue_mtx);

        // 4. 检查队列是否为空
        if (state.send_queue.empty()) {
            if (state.is_writing) {
                state.is_writing = false;

                ulock_send.unlock(); // 临时释放内部锁

                // 调用 epoll_ctl 是安全的，因为 'ac' 仍然存活，
                // 'state' 对象仍然存在（即使我们暂时不访问它）。
                update_epoll_events(sock, EPOLLIN | EPOLLET); // 移除 EPOLLOUT

                ulock_send.lock(); // 重新获取内部锁
            }

            return true;
        }

        bool is_alive = process_send_queue(sock, state, ulock_send);

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
        std::unique_lock<std::mutex> lock(mtx_epoll_ctl); // 保护 epoll_ctl
        struct epoll_event event;
        event.events = events;
        event.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
            perror("epoll_ctl_mod failed");
            std::cerr << "[" << now_ms_time_gen_str() << "] [错误] epoll_ctl_mod for FD: " << fd << " with events: " << events << " failed: " << strerror(errno) << std::endl;
            // 实际应用中可能需要更复杂的错误处理，例如关闭连接
        }
    }



    // 内部辅助函数，用于客户端映射管理（假设 mtx_climap 已加锁）
    void add_clientid(const std::string& id ,int sock){

        tbb::concurrent_hash_map<int, std::string>::accessor ac;
        if(!map_clientid.find(ac,sock)){
            map_clientid.insert(ac,sock);
            ac->second=id;
        }
        else{
              std::cerr << "[" << now_ms_time_gen_str() << "] [Error] add_clientid FD '" << sock << "' ALREADY EXISTS " << std::endl;
        }

    }

    // 内部辅助函数
    void delete_clientid( int sock){
        tbb::concurrent_hash_map<int, std::string>::accessor ac;
        if(map_clientid.find(ac,sock)){
            map_clientid.erase(sock);
        }
        else{
              std::cerr << "[" << now_ms_time_gen_str() << "] [Error] delete_clientid : FD '" << sock << "' NOT FOUND " << std::endl;
        }
    }

    bool process_message(ClientState& state, int sock, Mybyte&& body,ClientStateMap::accessor& ac) {


        MessageParser mp(body);

        bool success=0;
        if (!state.id_registered) {


            if (static_cast<Eve>(state.event_type)  == Eve::CLIENT_REQUEST_REGISTER) {

                std::string userid=mp.read_string();

                if (userid.empty()) {
                    std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 客户端 (FD: " << sock
                              << ") 发送了空的注册ID。" << std::endl;
                    return false; // ID 为空，关闭连接
                }

                add_clientid(userid, sock);
                state.clientid=userid;


                state.id_registered = true; // 标记为已注册

                success=1;
                std::cerr << "[" << now_ms_time_gen_str() << "] [信息] 客户端 '" << userid
                          << "' (FD: " << sock << ") 注册成功。" << std::endl;




            } else {
                // 错误：未注册的客户端发送了非注册事件
                std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 客户端 (FD: " << sock
                          << ") 尚未注册，但发送了事件: " << state.event_type << std::endl;
               // 非法操作，关闭连接
            }


            auto coid= state.correlation_id;
            ac.release();

            uint16_t placehold2 = UINT16_MAX;
            MessageBuilder mb;
            mb.append(success);
            send_msg(sock, Eve::SERVER_RESPONSE_REGISTER,coid, placehold2, mb.data);
            return success;

        } else {
            // 状态：已注册
            // 处理常规事件

            if (static_cast<Eve>(state.event_type)== MYMQ::EventType::CLIENT_REQUEST_REGISTER) {
                // 错误：已注册的客户端尝试重复注册
                std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 客户端 (FD: " << sock
                          << ") 尝试重复注册。" << std::endl;
                return false; // 重复注册，关闭连接
            }

            // --- 常规事件处理 ---


            auto event_type=state.event_type;
            auto coid=state.correlation_id;
            auto ack_level=state.ack_level;
            auto clientid=state.clientid;
            ac.release();
             auto real_body= mp.read_uchar_vector();
            handle_event(clientid,event_type, std::move(real_body), sock ,coid,ack_level );
            return true;
        }
    }

    bool handle_client(int sock) {
        ClientStateMap::accessor ac;
        if (!map_client_states.find(ac, sock)) {
            std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到未知FD的EPOLLOUT事件: "
                      << sock << std::endl;
            return false;
        }

        ClientState& state = *(ac->second);



        while (true) {
            ssize_t bytes_read = 0;
            switch (state.current_state) {

                // READING_ID_LENGTH 和 READING_ID 状态已移除

                case ClientState::READING_HEADER: {
                    // 只有当消息头未完全读取时才调用 recv
                    if (state.bytes_read_in_header < HEADER_SIZE) {
                        bytes_read = recv(sock, reinterpret_cast<char*>(state.header_buffer.data() + state.bytes_read_in_header),
                                          HEADER_SIZE - state.bytes_read_in_header, 0);

                        if (bytes_read > 0) {
                            state.bytes_read_in_header += bytes_read;

                            // 消息头刚刚读完
                            if (state.bytes_read_in_header == HEADER_SIZE) {
                                // --- 解析消息头 ---
                                uint32_t total_length_net;
                                memcpy(&total_length_net, state.header_buffer.data(), sizeof(uint32_t));
                                uint32_t total_length = ntohl(total_length_net);

                                uint16_t event_type_net;
                                memcpy(&event_type_net, state.header_buffer.data() + sizeof(uint32_t), sizeof(uint16_t));
                                state.event_type = ntohs(event_type_net);

                                uint32_t correlation_id_net;
                                memcpy(&correlation_id_net, state.header_buffer.data() + sizeof(uint32_t) + sizeof(uint16_t), sizeof(uint32_t));
                                state.correlation_id = ntohl(correlation_id_net);

                                uint16_t ack_level_net;
                                memcpy(&ack_level_net, state.header_buffer.data() + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t), sizeof(uint16_t));
                                state.ack_level = ntohs(ack_level_net);

                                // --- 验证消息头 ---
                                if (total_length < HEADER_SIZE) {
                                    std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到畸形消息 (FD: " << sock << "): total_length (" << total_length << ") < HEADER_SIZE (" << HEADER_SIZE << ")" << std::endl;
                                    return false;
                                }
                                state.expected_body_length = total_length - HEADER_SIZE;

                                if (state.expected_body_length > msg_body_limit) {
                                    std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到过大消息体 (FD: " << sock << "): " << state.expected_body_length << " bytes, limit is " << msg_body_limit << std::endl;
                                    return false;
                                }

                                // --- 状态转换 ---
                                if (state.expected_body_length == 0) {
                                    // 没有消息体，立即处理

                                    if (!process_message(state, sock, Mybyte{},ac)) {
                                        return false; // 处理失败，关闭连接
                                    }
                                    // 重置状态以处理下一条消息
                                    state.bytes_read_in_header = 0;
                                    // current_state 保持 READING_HEADER，继续循环
                                } else {
                                    // 有消息体，准备读取
                                    state.body_buffer.clear();
                                    state.bytes_read_in_body = 0;
                                    state.current_state = ClientState::READING_BODY; // 状态转换
                                }
                            }
                        } else if (bytes_read == 0) {
                            std::cerr << "[" << now_ms_time_gen_str() << "] [信息] 客户端 (FD: " << sock << ") 在读取头部时断开连接。" << std::endl;
                            return false;
                        } else { // bytes_read < 0
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                return true; // 数据未就绪，等待下次epoll/select
                            }
                            perror("read header failed");
                            return false;
                        }
                    } else {
                        // 如果 header 已经读满 (例如上一个循环中读完但没有 body)
                        // 确保我们转换状态或重置
                        if (state.expected_body_length > 0) {
                            state.current_state = ClientState::READING_BODY;
                        } else {
                            // 这种情况不应该发生，因为无 body 消息会立即重置
                            state.bytes_read_in_header = 0;
                        }
                    }
                    break;
                } // 结束 case READING_HEADER

                case ClientState::READING_BODY: {
                    // 确保 buffer 至少有预期那么大
                    if (state.body_buffer.size() < state.expected_body_length) {
                        state.body_buffer.resize(state.expected_body_length);
                    }

                    if (state.bytes_read_in_body < state.expected_body_length) {
                        bytes_read = recv(sock, reinterpret_cast<char*>(state.body_buffer.data() + state.bytes_read_in_body),
                                          state.expected_body_length - state.bytes_read_in_body, 0);

                        if (bytes_read > 0) {
                            state.bytes_read_in_body += bytes_read;

                            // 消息体刚刚读完
                            if (state.bytes_read_in_body == state.expected_body_length) {
                                // --- 处理消息 ---
                                // 消息体完全接收，调用 process_message
                                // 我们移动 body_buffer 以避免复制
                                if (!process_message(state, sock, std::move(state.body_buffer),ac)) {
                                    return false; // 处理失败，关闭连接
                                }

                                // --- 重置状态 ---
                                // 为下一条消息做准备
                                state.current_state = ClientState::READING_HEADER;
                                state.bytes_read_in_header = 0;
                                state.expected_body_length = 0;
                                // state.body_buffer 已被移动，下次使用时在READING_HEADER中会clear()
                                state.bytes_read_in_body = 0;
                            }
                        } else if (bytes_read == 0) {
                            std::cerr << "[" << now_ms_time_gen_str() << "] [信息] 客户端 (FD: " << sock << ") 在读取消息体时断开连接。" << std::endl;
                            return false;
                        } else { // bytes_read < 0
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                return true; // 数据未就绪，等待下次
                            }
                            perror("read body failed");
                            return false;
                        }
                    }
                    break;
                } // 结束 case READING_BODY

                default:
                    std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 未知客户端状态 (FD: " << sock << ") state: " << state.current_state << std::endl;
                    return false;
            } // 结束 switch

            // 如果我们没有因为 EAGAIN, 0 或 error 退出，
            // 并且 bytes_read > 0，while(true) 循环将继续处理
            // (例如：处理完一个无body消息，立即尝试读下一个header)

        } // 结束 while(true)
    }


    // 将客户端消息回调函数提交到线程池
    void handle_event(std::string clientid,short event_type, Mybyte msg_body, int client_fd,uint32_t correlation_id,uint16_t ack_level){
        ClientMessageCallback curr_cb;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_callback);
            curr_cb = client_msg_callback_;
        }

        if(curr_cb){

            curr_cb(client_fd,std::move(clientid), event_type,correlation_id,ack_level, std::move(msg_body));

        }
    }


private:
    int epfd_;
    const int MAX_EVENT_NUM = 1000;
    std::atomic<bool> running_{0};
    int server_fd;
    struct sockaddr_in address;


    tbb::concurrent_hash_map<int, std::unique_ptr<ClientState>> map_client_states;

     tbb::concurrent_hash_map<int, std::string> map_clientid;
    ClientMessageCallback client_msg_callback_;
    std::shared_mutex mtx_callback; // 保护 client_msg_callback_

    size_t msg_body_limit;
    int PORT;
    size_t HEADER_SIZE;
    int check_connect_liveness_ms;

    std::mutex mtx_epoll_ctl;


};

#endif
