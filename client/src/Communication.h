#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include "Config_manager.h"
#include "Serialize.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <optional>
#include <future>
#include <memory>
#include "Timer.h"
#include "MYMQ_innercodes.h"
#include "zlib.h"
#include "readerwriterqueue.h"
#include "tbb/concurrent_hash_map.h"
#include "ThreadPool.h"
#include"Printqueue.h"
#include<openssl/ssl.h>
#include<openssl/err.h>


// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

using Mybyte = std::vector<unsigned char>;
using MP = MessageParser;
using Eve=MYMQ::EventType;
using ClientState=MYMQ::MYMQ_Client::ClientState;
using PendingMessage=MYMQ::PendingMessage;
using ResponseCallback=MYMQ::ResponseCallback;

// 辅助函数（保持不变）
inline std::string now_ms_time_gen_str() {
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm p_tm_storage;
    std::tm* p_tm = nullptr;
#ifdef _WIN32
    if (localtime_s(&p_tm_storage, &now_c) == 0) p_tm = &p_tm_storage;
#else
    if (localtime_r(&now_c, &p_tm_storage) != nullptr) p_tm = &p_tm_storage;
#endif
    if (p_tm == nullptr) return "[Time Error]";
    std::stringstream ss_full;
    ss_full << std::put_time(p_tm, "%Y-%m-%d %H:%M:%S");
    auto duration_since_epoch = now.time_since_epoch();
    auto seconds_part = std::chrono::duration_cast<std::chrono::seconds>(duration_since_epoch);
    auto fractional_seconds = duration_since_epoch - seconds_part;
    auto milliseconds_part = std::chrono::duration_cast<std::chrono::milliseconds>(fractional_seconds);
    ss_full << "." << std::setfill('0') << std::setw(3) << milliseconds_part.count();
    return ss_full.str();
}


class Communication_client {
public:
    // [!_MODIFIED_!] Callback simplified to match your core class's handler


    struct ClientState {
        enum State { READING_HEADER, READING_BODY };
        State current_state = READING_HEADER;
        std::vector<unsigned char> header_buffer;
        std::vector<unsigned char> body_buffer;
        uint32_t expected_body_length = 0;
        uint16_t event_type = 0;
        uint32_t correlation_id = 0;
        uint16_t ack_level = 0;
        size_t received_bytes = 0;

        ClientState(size_t header_size) : header_buffer(header_size) {}

        void reset(size_t header_size) {
            current_state = READING_HEADER;
            header_buffer.assign(header_size, 0);
            body_buffer.clear();
            expected_body_length = 0;
            event_type = 0;
            correlation_id = 0;
            ack_level = 0;
            received_bytes = 0;
        }
    };

    Communication_client(const std::string& path_) :
        path(path_), client_state(0) {
    }

    ~Communication_client() {
        stop();
    }

    void init() {
        if (running_.load()) return;


        {

            Config_manager config_mgr(path + "\\config\\communication.ini");
            server_IP = config_mgr.getstring("host");
            port = config_mgr.getint("port");
            HEADER_SIZE = MYMQ::HEADER_SIZE;
            client_state.reset(HEADER_SIZE);
            auto send_queue_size = config_mgr.getull("send_queue_size");
            if(!inrange(send_queue_size,2048,16384)){
                send_queue_size=MYMQ::send_queue_size_DEFAULT;
            }
            {
                moodycamel::ReaderWriterQueue<PendingMessage> tmp_queue(send_queue_size);
                send_queue = std::move(tmp_queue);
            }
            request_timeout_s=config_mgr.get_size_t("request_timeout_s");

        }


        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("Unable to create SSL context");

        if (SSL_CTX_set_ciphersuites(ctx_, "TLS_AES_256_GCM_SHA384") != 1) {
            throw std::runtime_error("Error setting TLS 1.3 ciphersuites");
        }

        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);

        // 如果是自签名证书或测试环境，可以暂时跳过验证（生产环境建议开启验证）
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, NULL);
        // ---------------------------------------------------

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) throw std::runtime_error("WSAStartup failed: " + std::to_string(iResult));

        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) {
            throw std::runtime_error("Error at socket(): " + std::to_string(WSAGetLastError()));
        }

        u_long mode = 1; // Non-blocking
        if (ioctlsocket(clientSocket, FIONBIO, &mode) != 0) {
            int lastError = WSAGetLastError();
            closesocket(clientSocket);
            WSACleanup();
            throw std::runtime_error("ioctlsocket() failed: " + std::to_string(lastError));
        }

        // [NEW] 2. 创建 SSL 对象并绑定 Socket
        // 注意：此时还没有握手，只是将 SSL 结构绑定到 socket fd 上
        // ---------------------------------------------------
        ssl_ = SSL_new(ctx_);
        if (!ssl_) throw std::runtime_error("Unable to create SSL object");

        // 设置 SNI (Server Name Indication)，某些服务器如果没有这个会握手失败
        SSL_set_tlsext_host_name(ssl_, server_IP.c_str());

        // 将 SSL 对象绑定到 Windows 的 socket
        SSL_set_fd(ssl_, clientSocket);

        // 设为连接状态 (Client Mode)
        SSL_set_connect_state(ssl_);
        // ---------------------------------------------------

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);

        if (inet_pton(AF_INET, server_IP.c_str(), &serverAddr.sin_addr) != 1) {
            closesocket(clientSocket);
            WSACleanup();
            throw std::runtime_error("Invalid IP address or inet_pton failed for IP: " + server_IP);
        }

        // [IMPORTANT] TCP 连接逻辑
        iResult = connect(clientSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
        if (iResult == SOCKET_ERROR) {
            int lastError = WSAGetLastError();
            if (lastError == WSAEWOULDBLOCK || lastError == WSAEINPROGRESS) {
                connect_pending_.store(true);
                // 注意：这里不能立即进行 SSL_connect，因为 TCP 还没通
            } else {
                closesocket(clientSocket);
                WSACleanup();
                throw std::runtime_error("Connect failed with error: " + std::to_string(lastError));
            }
        } else {
            std::cout << "Connection with server built" << std::endl;
            connection_established_.store(true);
            connect_pending_.store(false);

            // 如果 TCP 既然已经立即连上了（极少见，但在本地可能发生），
            // 这里依然不能直接阻塞调用 SSL_connect，建议留给 io_loop 处理，
            // 或者在这里尝试一次非阻塞握手。
        }

        running_.store(true);
        io_thread_ = std::thread([this] { io_loop(); });
    }

    void set_ACK_level(MYMQ::ACK_Level set){
        ack_level=set;
    }

    // [!_MODIFIED_!] Accepts the new, simplified ResponseCallback
    uint32_t send_msg(short event_type, const Mybyte& msg_body, ResponseCallback handler) {
        MessageBuilder mb;
        uint32_t coid = Correlation_ID.fetch_add(1); // Get new COID
        uint32_t total_length_on_wire = static_cast<uint32_t>(HEADER_SIZE +sizeof(uint32_t)+ msg_body.size());
        mb.reserve(total_length_on_wire);

        ////HEADER
        mb.append_uint32(total_length_on_wire);
        mb.append_uint16(event_type);
        mb.append_uint32(coid);
        mb.append_uint16(static_cast<uint16_t>(ack_level));
        ////HEADER
        mb.append_uchar_vector(msg_body);

        auto full_message = mb.data;

        send_queue.try_emplace(std::move(full_message), coid, std::move(handler));
        send_pending.store(true);
        return coid;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }

        int iResult = WSACleanup();
        if (iResult != 0) {
            std::cerr << "[" << now_ms_time_gen_str() << "] WSACleanup failed: " << iResult << std::endl;
        }
    }

    void set_clientid(const std::string clientid) {
        client_id_str = clientid;
    }

private:
    // 尝试发送队列中的消息 (logic unchanged)
    void attemped_send() {
        // [修改 1] 必须等待 SSL 握手完成才能发送数据
        if (!connection_established_.load() || !ssl_handshaked_) {
            // 如果只是握手还没好，保持 send_pending 为 true，下一轮继续尝试
            // send_pending.store(false); // 不要轻易关掉，否则可能漏发
            return;
        }

        // 1. 优先发送 client_id 消息
        if (client_id_message_to_send_.has_value()) {
            auto& pair = client_id_message_to_send_.value();
            auto& msg = pair.first;
            auto& off = pair.second;

            const char* byte_to_send = reinterpret_cast<const char*>(msg.data() + off);
            size_t bytes_remaining = msg.size() - off;

            // [修改 2] 使用 SSL_write
            int res = SSL_write(ssl_, byte_to_send, static_cast<int>(bytes_remaining));

            if (res <= 0) {
                // [修改 3] SSL 错误处理逻辑
                int err = SSL_get_error(ssl_, res);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    // 底层缓冲区满，稍后重试 (相当于 WSAEWOULDBLOCK)
                    return;
                } else {
                    // 真正的错误或连接关闭
                    std::cerr << "[" << now_ms_time_gen_str() << "] SSL_write failed for client_id. Error code: " << err << std::endl;
                    ERR_print_errors_fp(stderr); // 打印详细 OpenSSL 错误栈
                    client_id_message_to_send_.reset();
                    running_.store(false);
                    return;
                }
            } else {
                // 发送成功 res > 0
                off += res;
                if (off == msg.size()) {
                    std::cout << "[" << now_ms_time_gen_str() << "] Client ID message fully sent." << std::endl;
                    client_id_message_to_send_.reset();
                }
            }
        }

        // 如果 Client ID 还没发完，就先别发后面的
        if (client_id_message_to_send_.has_value()) {
            send_pending.store(true);
            return;
        }

        if (!is_registered.load()) {
            send_pending.store(send_queue.peek() != nullptr);
            return;
        }

        // 2. 发送队列消息
        while (true) {
            PendingMessage* current_msg_ptr = send_queue.peek();
            if (current_msg_ptr == nullptr) break;

            auto& msg = current_msg_ptr->message_bytes;
            auto& off = current_msg_ptr->offset;

            const char* byte_to_send = reinterpret_cast<const char*>(msg.data() + off);
            size_t bytes_remaining = msg.size() - off;

            // [修改 4] 使用 SSL_write
            int res = SSL_write(ssl_, byte_to_send, static_cast<int>(bytes_remaining));

            if (res <= 0) {
                int err = SSL_get_error(ssl_, res);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    // 缓冲区满，退出循环，等待下次 write 事件
                    break;
                } else {
                    std::cerr << "[" << now_ms_time_gen_str() << "] SSL_write failed. Error code: " << err << std::endl;
                    ERR_print_errors_fp(stderr);

                    // 移除坏掉的消息并停止
                    PendingMessage dummy;
                    send_queue.try_dequeue(dummy);
                    running_.store(false);
                    return;
                }
            } else {
                // 发送成功
                off += res;
                if (off == msg.size()) {
                    // --- 消息发送完毕逻辑 (保持原样) ---
                    tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
                    map_wait_responces.insert(acc, current_msg_ptr->coid);
                    acc->second = std::move(current_msg_ptr->handler);
                    acc.release();

                    // 启动定时器
                    auto coid = current_msg_ptr->coid;
                    timer.commit_s([this, coid]{
                        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc_timer;
                        if( map_wait_responces.find(acc_timer, coid)){
                            std::cerr << "[Request Time out] Correlationid : " << std::to_string(coid) << std::endl;
                            map_wait_responces.erase(acc_timer);
                        }
                    }, request_timeout_s, request_timeout_s, 1);

                    // 将消息出队
                    PendingMessage dummy;
                    send_queue.try_dequeue(dummy);
                }
                else {
                    // 没发完 (Partial write)，但 OpenSSL 可能因为底层机制而返回
                    // 此时不能 continue 死循环，因为如果底层还是满的，会空转 CPU
                    // 对于 SSL 来说，最好 break 出去重新让 select 调度
                    break;
                }
            }
        }

        send_pending.store(client_id_message_to_send_.has_value() || send_queue.peek() != nullptr);
    }


    void send_client_id_on_connect() {
        if (client_id_sent_.load()) return;


        MessageBuilder mb_full;
        short event_type = static_cast<short>(Eve::CLIENT_REQUEST_REGISTER);
        uint32_t total_length_on_wire = static_cast<uint32_t>(HEADER_SIZE + sizeof(uint32_t) +client_id_str.size());
        mb_full.reserve(total_length_on_wire);

        ////HEADER
        mb_full.append_uint32(total_length_on_wire);
        mb_full.append_uint16(event_type);
        mb_full.append_uint32(Correlation_ID++); // Fire-and-forget
        mb_full.append_uint16(static_cast<uint16_t>(ack_level));
        ////HEADER
        mb_full.append_string(client_id_str);

        auto full_message = mb_full.data;
        client_id_message_to_send_ = std::make_pair(std::move(full_message), 0);
        send_pending.store(true);
        client_id_sent_.store(true);
    }

    void io_loop() {
        FD_SET read_fds, write_fds;
        timeval timeout;

        while (running_) {
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);

            // ============================================================
            // 1. 构建 select 监听集合 (根据当前状态机决定监听什么)
            // ============================================================

            // 状态 A: TCP 正在连接中 (还没有建立 TCP 连接)
            if (connect_pending_.load()) {
                FD_SET(clientSocket, &write_fds);
            }
            // 状态 B: TCP 已连接，但 SSL 正在握手中
            else if (connection_established_.load() && !ssl_handshaked_) {
                // 握手阶段：完全听从 OpenSSL 的指挥 (它想读就监听读，想写就监听写)
                if (ssl_want_read_) FD_SET(clientSocket, &read_fds);
                if (ssl_want_write_) FD_SET(clientSocket, &write_fds);
            }
            // 状态 C: SSL 握手完成，进入正常业务数据传输
            else if (ssl_handshaked_) {
                // 始终监听读 (服务器可能随时推数据或断开)
                FD_SET(clientSocket, &read_fds);

                // 只有当我们有数据要发送时，才监听写
                if (send_pending.load()) {
                    FD_SET(clientSocket, &write_fds);
                }
            }

            // ============================================================
            // 2. 执行 Select
            // ============================================================
            timeout.tv_sec = 0;
            timeout.tv_usec = 10000; // 10ms

            // 注意：Windows下 select 第一个参数会被忽略，但在 Linux 下需要是 maxfd + 1
            int result = select(0, &read_fds, &write_fds, nullptr, &timeout);

            if (result == SOCKET_ERROR) {
                int lastError = WSAGetLastError();
                if (lastError == WSAEINTR) continue;
                std::cerr << "[" << now_ms_time_gen_str() << "] select() failed with error: " << lastError << std::endl;
                running_.store(false);
                break;
            }

            if (result == 0) continue;

            // ============================================================
            // 3. 处理 TCP 连接完成事件
            // ============================================================
            if (connect_pending_.load() && FD_ISSET(clientSocket, &write_fds)) {
                int opt_val;
                int opt_len = sizeof(opt_val);
                if (getsockopt(clientSocket, SOL_SOCKET, SO_ERROR, (char*)&opt_val, &opt_len) == SOCKET_ERROR) {
                    std::cerr << "[" << now_ms_time_gen_str() << "] getsockopt(SO_ERROR) failed: " << WSAGetLastError() << std::endl;
                    running_.store(false);
                    break;
                }

                if (opt_val == 0) {
                    std::cout << "[" << now_ms_time_gen_str() << "] TCP Connected. Starting SSL Handshake..." << std::endl;
                    connection_established_.store(true);
                    connect_pending_.store(false);

                    // [关键点] TCP 连上后，立即请求一次“写”权限来触发 SSL_connect
                    ssl_want_write_ = true;
                    ssl_want_read_ = false;
                } else {
                    std::cerr << "[" << now_ms_time_gen_str() << "] Non-blocking connect failed with error: " << opt_val << std::endl;
                    running_.store(false);
                    break;
                }
            }

            // ============================================================
            // 4. 处理 SSL 握手 (Handshake)
            // ============================================================
            if (connection_established_.load() && !ssl_handshaked_) {
                // 只要 socket 可读或可写，且 OpenSSL 之前要求了对应的事件，就尝试继续握手
                bool ready_read = FD_ISSET(clientSocket, &read_fds);
                bool ready_write = FD_ISSET(clientSocket, &write_fds);

                if ((ready_read && ssl_want_read_) || (ready_write && ssl_want_write_)) {
                    int ret = SSL_connect(ssl_);

                    if (ret == 1) {
                        std::cout << "[" << now_ms_time_gen_str() << "] SSL/TLS Handshake Success!" << std::endl;
                        ssl_handshaked_ = true;
                        // 握手成功，重置握手状态标志，避免干扰后续业务
                        ssl_want_read_ = false;
                        ssl_want_write_ = false;
                    } else {
                        int err = SSL_get_error(ssl_, ret);
                        if (err == SSL_ERROR_WANT_READ) {
                            ssl_want_read_ = true;
                            ssl_want_write_ = false;
                        } else if (err == SSL_ERROR_WANT_WRITE) {
                            ssl_want_write_ = true;
                            ssl_want_read_ = false;
                        } else {
                            std::cerr << "[" << now_ms_time_gen_str() << "] SSL Handshake Failed. Error code: " << err << std::endl;
                            ERR_print_errors_fp(stderr);
                            running_.store(false);
                            break;
                        }
                    }
                }
            }

            // ============================================================
            // 5. 处理 业务逻辑 (仅在 SSL 握手成功后)
            // ============================================================
            if (ssl_handshaked_) {

                // --- A. 自动触发发送 Client ID ---
                // 握手刚完成时，client_id_sent_ 为 false，立即触发构建消息
                if (!client_id_sent_.load()) {
                    send_client_id_on_connect();
                    // 消息构建完后，立刻尝试发送，不需要等下一轮 select
                    // 因为 send_client_id_on_connect 只是把数据放进内存，attemped_send 负责 SSL_write
                    attemped_send();
                }

                // --- B. 处理写事件 (发送队列中的数据) ---
                else if (FD_ISSET(clientSocket, &write_fds) && send_pending.load()) {
                    // 此时 attemped_send 内部已经是 SSL_write 了
                    attemped_send();
                }

                // --- C. 处理读事件 (接收服务器响应) ---
                if (FD_ISSET(clientSocket, &read_fds)) {
                    // 此时 handle_incoming_data 内部已经是 SSL_read 了
                    if (!handle_incoming_data()) {
                        running_.store(false);
                        break;
                    }
                }
            }
        }

        std::cout << "[" << now_ms_time_gen_str() << "] Client I/O loop stopped." << std::endl;
    }
    bool handle_incoming_data() {
        char buffer[4096];

        // [修改 1] 使用 SSL_read 替代 recv
        int bytes_received = SSL_read(ssl_, buffer, sizeof(buffer));

        // [修改 2] SSL 特有的错误/状态处理
        if (bytes_received <= 0) {
            int err = SSL_get_error(ssl_, bytes_received);

            // 类似于 WSAEWOULDBLOCK，表示底层缓存空了，等待下次 select
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return true;
            }
            // 服务器优雅关闭了 SSL 连接
            else if (err == SSL_ERROR_ZERO_RETURN) {
                std::cout << "[" << now_ms_time_gen_str() << "] Server closed SSL connection gracefully." << std::endl;
                return false;
            }
            // 真正的错误
            else {
                std::cerr << "[" << now_ms_time_gen_str() << "] SSL_read failed. Error code: " << err << std::endl;
                ERR_print_errors_fp(stderr); // 打印 OpenSSL 错误队列
                return false;
            }
        }

        // --------------------------------------------------------------
        // 下面的逻辑是你原有的代码，完全保持不变 (解析协议、处理粘包)
        // --------------------------------------------------------------

        // 跟踪我们在 buffer 中处理了多少字节
        int bytes_processed = 0;

        // 只要 buffer 中还有未处理的数据，就持续循环
        while (bytes_processed < bytes_received) {

            if (client_state.current_state == ClientState::READING_HEADER) {
                // 1. 计算还需要多少字节来填满头部
                int bytes_needed = HEADER_SIZE - client_state.received_bytes;

                // 2. 计算此 buffer 中还剩多少字节可用
                int bytes_available = bytes_received - bytes_processed;

                // 3. 确定我们这次实际能复制多少（取两者中的较小值）
                int bytes_to_copy = (bytes_needed < bytes_available) ? bytes_needed : bytes_available;

                // 4. 批量复制数据到 header_buffer
                memcpy(client_state.header_buffer.data() + client_state.received_bytes,
                       buffer + bytes_processed,
                       bytes_to_copy);

                // 5. 更新计数器
                client_state.received_bytes += bytes_to_copy;
                bytes_processed += bytes_to_copy;

                // 6. 检查头部是否已完整
                if (client_state.received_bytes == HEADER_SIZE) {
                    // --- 你的头部解析逻辑 ---
                    uint32_t total_length_net;
                    memcpy(&total_length_net, client_state.header_buffer.data(), sizeof(uint32_t));
                    uint32_t total_length = ntohl(total_length_net);

                    if (total_length < HEADER_SIZE) {
                        std::cerr << "[" << now_ms_time_gen_str() << "] [错误] 收到畸形消息: total_length (" << total_length << ") < HEADER_SIZE (" << HEADER_SIZE << ")" << std::endl;
                        return false; // 致命协议错误
                    }

                    uint16_t event_type_net;
                    memcpy(&event_type_net, client_state.header_buffer.data() + sizeof(uint32_t), sizeof(uint16_t));
                    client_state.event_type = ntohs(event_type_net);

                    uint32_t correlation_id_net;
                    memcpy(&correlation_id_net, client_state.header_buffer.data() + sizeof(uint32_t) + sizeof(uint16_t), sizeof(uint32_t));
                    client_state.correlation_id = ntohl(correlation_id_net);

                    uint16_t ack_level_net;
                    memcpy(&ack_level_net, client_state.header_buffer.data() + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t), sizeof(uint16_t));
                    client_state.ack_level = ntohs(ack_level_net);

                    client_state.expected_body_length = total_length - HEADER_SIZE;
                    // --- 头部解析逻辑结束 ---

                    if (client_state.expected_body_length > 0) {
                        client_state.body_buffer.resize(client_state.expected_body_length);
                        client_state.current_state = ClientState::READING_BODY;
                        client_state.received_bytes = 0; // 重置计数器，准备接收 body
                    } else {
                        // 没有 body，直接处理事件
                        handle_event(client_state.event_type, client_state.correlation_id, client_state.ack_level, Mybyte{});
                        client_state.reset(HEADER_SIZE); // 重置状态机
                    }
                }
            }
            else if (client_state.current_state == ClientState::READING_BODY) {
                // 1. 计算还需要多少字节来填满 body
                int bytes_needed = client_state.expected_body_length - client_state.received_bytes;

                // 2. 计算此 buffer 中还剩多少字节可用
                int bytes_available = bytes_received - bytes_processed;

                // 3. 确定我们这次实际能复制多少
                int bytes_to_copy = (bytes_needed < bytes_available) ? bytes_needed : bytes_available;

                // 4. 批量复制数据到 body_buffer
                memcpy(client_state.body_buffer.data() + client_state.received_bytes,
                       buffer + bytes_processed,
                       bytes_to_copy);

                // 5. 更新计数器
                client_state.received_bytes += bytes_to_copy;
                bytes_processed += bytes_to_copy;

                // 6. 检查 body 是否已完整
                if (client_state.received_bytes == client_state.expected_body_length) {
                    // --- 你的 body 处理逻辑 ---
                    if (!is_registered) {
                        if (static_cast<Eve>(client_state.event_type) == MYMQ::EventType::SERVER_RESPONSE_REGISTER) {
                            MP mp(client_state.body_buffer);
                            auto content=  mp.read_uchar_vector();
                            MP mp_content(content);
                            auto succ= mp_content.read_bool();
                            std::string resp;
                            resp+="Register result : '"+client_id_str+"' register ";
                            if(succ){
                                is_registered = 1;
                                resp+="success";
                            }
                            else{
                                resp+="failed";
                            }

                            out(resp);

                        } else {
                            std::cerr << "[" << now_ms_time_gen_str() << "] Not yet registered but received  Event :" << MYMQ::to_string(static_cast<Eve>(client_state.event_type)) << std::endl;
                        }
                    } else {
                        if (static_cast<Eve>(client_state.event_type) == MYMQ::EventType::SERVER_RESPONSE_REGISTER) {
                            std::cerr << "[" << now_ms_time_gen_str() << "] Already registered but received  Event 'REGISTER'" << std::endl;
                        } else {
                            if (client_state.event_type == static_cast<uint16_t>(MYMQ::EventType::SERVER_RESPONSE_PULL_DATA)) {
                                handle_event(client_state.event_type, client_state.correlation_id, client_state.ack_level, std::move(client_state.body_buffer));
                            } else {
                                MP mp(client_state.body_buffer);
                                handle_event(client_state.event_type, client_state.correlation_id, client_state.ack_level, std::move(mp.read_uchar_vector()));
                            }
                        }
                    }
                    client_state.reset(HEADER_SIZE);
                }
            }
        }

        return true;
    }
    void handle_event(uint16_t eventtype, uint32_t correlation_id, uint16_t ack_level, Mybyte msg_body) {
        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;

        if (map_wait_responces.find(acc, correlation_id)) {
            auto cb = std::move(acc->second);
            map_wait_responces.erase(acc);
            cb(eventtype,std::move(msg_body) );
   }
        else {
            std::cerr << "[" << now_ms_time_gen_str() << "] Received response for unknown or expired correlation_id: "
                      << correlation_id
                      << " Event: " << MYMQ::to_string(static_cast<Eve>(eventtype)) << std::endl;
        }
    }


    bool inrange(size_t obj,size_t min,size_t max){
        return (obj<=max&&obj>=min);
    }

    void cerr(const std::string& str){
        Printqueue::instance().out(str,1,0);
    }

    void out(const std::string& str){
        Printqueue::instance().out(str,0,0);
    }
private:
    std::string path;
    std::string server_IP;
    int port;
    uint16_t HEADER_SIZE;
    size_t request_timeout_s;

    SOCKET clientSocket;
    std::atomic<bool> running_{false};
    std::string client_id_str;
    std::thread io_thread_;

   std::atomic<bool> is_registered=0;

    ClientState client_state;

    moodycamel::ReaderWriterQueue<PendingMessage> send_queue;
    std::optional<std::pair<Mybyte, size_t>> client_id_message_to_send_;

    std::atomic<bool> send_pending{false};
   std::atomic<bool> connect_pending_{false};
   std::atomic<bool> connection_established_{false};
    std::atomic<bool> client_id_sent_{false};

    MYMQ::ACK_Level ack_level=MYMQ::ACK_Level::ACK_PROMISE_ACCEPT;

    tbb::concurrent_hash_map<uint32_t, ResponseCallback> map_wait_responces;

    std::atomic<uint32_t> Correlation_ID{0};
    Timer timer;

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;

    // 用于标记 SSL 握手是否完成
    std::atomic<bool> ssl_handshake_complete_{false};

    bool ssl_handshaked_ = false;    // 标记 SSL 握手是否完成
    bool ssl_want_read_ = false;     // 握手过程中 SSL 是否在等待读
    bool ssl_want_write_ = true;     // 握手过程中 SSL 是否在等待写 (初始为 true 以启动握手)

};

#endif // COMMUNICATION_H
