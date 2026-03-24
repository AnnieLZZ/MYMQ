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
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <future>
#include <memory>
#include "Timer.h"
#include "MYMQ_innercodes.h"
#include "MYMQ_PublicCodes.h"
#include "zlib.h"
#include "readerwriterqueue.h"
#include "tbb/concurrent_hash_map.h"
#include "ThreadPool.h"
#include"Printqueue.h"
#include<openssl/ssl.h>
#include<openssl/err.h>
#include"Request_timeout_queue.h"



// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

using Mybyte = std::vector<unsigned char>;
using MP = MessageParser;
using Eve=MYMQ::EventType;
using ClientState=MYMQ::Client::ClientState;
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


namespace MYMQ {
namespace Network {

class Communication_client {
public:
    // [!_MODIFIED_!] Callback simplified to match your core class's handler


    struct ClientState {
        enum State { READING_HEADER, READING_BODY };
        State current_state = READING_HEADER;
        std::vector<unsigned char> header_buffer;
        std::shared_ptr<void> body_owner;
        unsigned char* body_ptr = nullptr;
        uint32_t expected_body_length = 0;
        uint16_t event_type = 0;
        uint32_t correlation_id = 0;
        uint16_t ack_level = 0;
        size_t received_bytes = 0;

        ClientState(size_t header_size) : header_buffer(header_size) {}

        void reset(size_t header_size) {
            current_state = READING_HEADER;
            header_buffer.assign(header_size, 0);
            body_owner.reset();
            body_ptr = nullptr;
            expected_body_length = 0;
            event_type = 0;
            correlation_id = 0;
            ack_level = 0;
            received_bytes = 0;
        }
    };

    Communication_client(const std::string& path_,size_t request_timeout_ms) :
        path(path_), client_state(0), default_request_timeout_ms_(request_timeout_ms) {
    }

    ~Communication_client() {
        stop();
    }

    void set_request_timeout_ms(size_t ms) {
        request_timeout_ms_override_ = ms;
    }

    void init() {
        if (running_.load()) return;
        network_fatal_.store(false, std::memory_order_release);


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


            IO_buffer_size=config_mgr.get_size_t("IO_buffer_size");
            IO_buffer.resize(IO_buffer_size);

        }

        {
            size_t request_timeout_ms = default_request_timeout_ms_;
            if (request_timeout_ms_override_) {
                request_timeout_ms = *request_timeout_ms_override_;
            } else {
                try {
                    Config_manager cm_sys(path + "\\config\\sys.ini");
                    request_timeout_ms = cm_sys.get_size_t("request_timeout_ms");
                } catch (...) {
                    request_timeout_ms = default_request_timeout_ms_;
                }
            }
            if(!inrange(request_timeout_ms,10,3600000)){
                request_timeout_ms = default_request_timeout_ms_;
            }
            request_timeout_timer = std::make_unique<RequestTimeoutQueue>(
                this->map_wait_responces,
                curr_flying_request_num,
                request_timeout_ms,
                [this] { notify_send_state(); }
            );
        }


        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("Unable to create SSL context");

        if (SSL_CTX_set_ciphersuites(ctx_, "TLS_AES_128_GCM_SHA256") != 1) {
            throw std::runtime_error("Error setting TLS 1.3 ciphersuites");
        }

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);

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

    bool get_is_register(){
        return is_registered.load();
    }

    enum class SendFailReason : uint8_t {
        None = 0,
        Stopped = 1,
        QueueFull = 2,
        InFlightFull = 3
    };

    bool send_msg(uint16_t event_type, const Mybyte& msg_body, ResponseCallback handler) {
        return try_send_msg(event_type, msg_body, std::move(handler), nullptr);
    }

    bool try_send_msg(uint16_t event_type, const Mybyte& msg_body, ResponseCallback handler, SendFailReason* fail_reason) {
        if (!running_.load(std::memory_order_acquire)) {
            if (fail_reason) *fail_reason = SendFailReason::Stopped;
            return false;
        }

        MessageBuilder mb;
        uint32_t coid = Correlation_ID.fetch_add(1);

        uint32_t total_length_on_wire = static_cast<uint32_t>(HEADER_SIZE + sizeof(uint32_t) + msg_body.size());
        mb.reserve(total_length_on_wire);

        mb.append_uint32(total_length_on_wire);
        mb.append_uint16(event_type);
        mb.append_uint32(coid);
        mb.append_uint16(static_cast<uint16_t>(ack_level));
        mb.append_uchar_vector(msg_body);

        bool need_wait_response = !(event_type == static_cast<uint16_t>(Eve::CLIENT_REQUEST_PUSH) &&
                                    ack_level == MYMQ::ACK_Level::ACK_NORESPONCE);

        if (need_wait_response) {

            tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
            if (map_wait_responces.insert(acc, coid)) {
                acc->second = std::move(handler); // 转移所有权，保存回调
            }
            curr_flying_request_num++;
            if (request_timeout_timer) {
                request_timeout_timer->add(coid);
            }
        }

        if (!send_queue.try_emplace(std::move(mb.data), coid, ResponseCallback{})) {
            if (need_wait_response) {
                tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
                if (map_wait_responces.find(acc, coid)) {
                    map_wait_responces.erase(acc);
                    curr_flying_request_num--;
                    notify_send_state();
                }
            }
            if (fail_reason) *fail_reason = SendFailReason::QueueFull;
            return false;
        }

        send_pending.store(true);
        notify_send_state();
        if (fail_reason) *fail_reason = SendFailReason::None;
        return true;
    }

    bool send_msg_blocking(uint16_t event_type, const Mybyte& msg_body, ResponseCallback handler, size_t max_in_flight_requests_num, std::chrono::milliseconds timeout = std::chrono::milliseconds(60000), SendFailReason* fail_reason = nullptr) {
        auto deadline = std::chrono::steady_clock::now() + timeout;

        while (true) {
            if (!running_.load(std::memory_order_acquire)) {
                if (fail_reason) *fail_reason = SendFailReason::Stopped;
                return false;
            }

            if (max_in_flight_requests_num > 0) {
                size_t curr_fly = curr_flying_request_num.load(std::memory_order_acquire);
                if (curr_fly >= max_in_flight_requests_num) {
                    std::unique_lock<std::mutex> lock(send_state_mtx_);
                    send_state_cv_.wait_until(lock, deadline, [&] {
                        return !running_.load(std::memory_order_acquire) ||
                               curr_flying_request_num.load(std::memory_order_acquire) < max_in_flight_requests_num;
                    });
                    if (std::chrono::steady_clock::now() >= deadline) {
                        if (fail_reason) *fail_reason = SendFailReason::InFlightFull;
                        return false;
                    }
                    continue;
                }
            }

            SendFailReason local_reason = SendFailReason::None;
            ResponseCallback handler_attempt = handler;
            if (try_send_msg(event_type, msg_body, std::move(handler_attempt), &local_reason)) {
                if (fail_reason) *fail_reason = SendFailReason::None;
                return true;
            }

            if (local_reason == SendFailReason::Stopped) {
                if (fail_reason) *fail_reason = SendFailReason::Stopped;
                return false;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                if (fail_reason) *fail_reason = local_reason;
                return false;
            }

            std::unique_lock<std::mutex> lock(send_state_mtx_);
            send_state_cv_.wait_until(lock, deadline);
        }
    }

    void stop() {
        if (!running_.load()) return;
        network_fatal_.store(false, std::memory_order_release);
        running_.store(false);
        notify_send_state();

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
        request_timeout_timer.reset();
    }

    bool inject_inflight_for_test(uint32_t coid, ResponseCallback handler) {
        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
        if (!map_wait_responces.insert(acc, coid)) {
            return false;
        }
        acc->second = std::move(handler);
        curr_flying_request_num++;
        notify_send_state();
        return true;
    }

    size_t drain_inflight_with_error(MYMQ_Public::CommonErrorCode code) {
        uint16_t net = htons(static_cast<uint16_t>(code));
        auto body = std::make_shared<Mybyte>(sizeof(uint16_t));
        std::memcpy(body->data(), &net, sizeof(uint16_t));

        std::vector<uint32_t> keys;
        keys.reserve(map_wait_responces.size());
        for (auto it = map_wait_responces.begin(); it != map_wait_responces.end(); ++it) {
            keys.push_back(it->first);
        }

        std::vector<ResponseCallback> callbacks;
        callbacks.reserve(keys.size());

        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
        for (uint32_t coid : keys) {
            if (map_wait_responces.find(acc, coid)) {
                callbacks.push_back(std::move(acc->second));
                map_wait_responces.erase(acc);
                curr_flying_request_num--;
            }
        }
        notify_send_state();

        for (auto& cb : callbacks) {
            try {
                cb(
                    static_cast<uint16_t>(MYMQ::EventType::EVENTTYPE_NULL),
                    MYMQ::OwnedBytes{body->data(), body->size(), body}
                );
            } catch (...) {
            }
        }
        return callbacks.size();
    }

    void set_clientid(const std::string clientid) {
        client_id_str = clientid;
    }
    
    void set_channel_role(MYMQ_Public::ChannelRole role) {
        channel_role_ = static_cast<uint16_t>(role);
    }

    void send_msg_prior(uint16_t event_type, const Mybyte& msg_body, ResponseCallback handler){

    }
    void get_curr_flying_request_num(size_t& num){
        num=curr_flying_request_num.load();
    }

private:
    void notify_send_state() {
        send_state_cv_.notify_all();
    }

    // 尝试发送队列中的消息 (logic unchanged)
    void attemped_send() {
        if (!connection_established_.load() || !ssl_handshaked_) {
            return;
        }

        // 1. 优先发送 client_id 消息
        if (client_id_message_to_send_.has_value()) {
            auto& pair = client_id_message_to_send_.value();
            auto& msg = pair.first;
            auto& off = pair.second;

            const char* byte_to_send = reinterpret_cast<const char*>(msg.data() + off);
            size_t bytes_remaining = msg.size() - off;

            size_t real_send_bytes=0;
            int res = SSL_write_ex(ssl_, byte_to_send, static_cast<int>(bytes_remaining),&real_send_bytes);

            if (res ==0) {
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
                    network_fatal_.store(true, std::memory_order_release);
                    running_.store(false);
                    notify_send_state();
                    return;
                }
            } else {
                // 发送成功 res =1
                off += real_send_bytes;
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

            size_t real_send_bytes = 0;
            int res = SSL_write_ex(ssl_, byte_to_send, static_cast<int>(bytes_remaining), &real_send_bytes);

            if (res == 0) {
                int err = SSL_get_error(ssl_, res);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    // 缓冲区满，等下次 Epoll
                    break;
                } else {
                    // 真正的错误
                    std::cerr << "[" << now_ms_time_gen_str() << "] SSL_write failed..." << std::endl;
                    ERR_print_errors_fp(stderr);


                    PendingMessage dummy;
                    send_queue.try_dequeue(dummy);
                    notify_send_state();

                    network_fatal_.store(true, std::memory_order_release);
                    running_.store(false);
                    notify_send_state();
                    return;
                }
            } else {
                // 发送成功部分或全部
                off += real_send_bytes;
                if (off == msg.size()) {
                    PendingMessage dummy;
                    send_queue.try_dequeue(dummy);
                    notify_send_state();

                } else {

                    break;
                }
            }
        }

        send_pending.store(client_id_message_to_send_.has_value() || send_queue.peek() != nullptr);
    }


    void send_client_id_on_connect() {
        if (client_id_sent_.load()) return;


        MessageBuilder payload;
        payload.append_uint16(1);
        payload.append_uint16(channel_role_);
        payload.append_string(client_id_str);

        const uint32_t total_length_on_wire =
            static_cast<uint32_t>(HEADER_SIZE + sizeof(uint32_t) + payload.data.size());

        MessageBuilder mb_full;
        mb_full.reserve(total_length_on_wire);
        mb_full.append_uint32(total_length_on_wire);
        mb_full.append_uint16(static_cast<uint16_t>(Eve::CLIENT_REQUEST_REGISTER));
        mb_full.append_uint32(Correlation_ID++);
        mb_full.append_uint16(static_cast<uint16_t>(ack_level));
        mb_full.append_uchar_vector(payload.data);

        auto full_message = std::move(mb_full.data);
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
                network_fatal_.store(true, std::memory_order_release);
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
                    network_fatal_.store(true, std::memory_order_release);
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
                    network_fatal_.store(true, std::memory_order_release);
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
                            network_fatal_.store(true, std::memory_order_release);
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
                        network_fatal_.store(true, std::memory_order_release);
                        running_.store(false);
                        break;
                    }
                }
            }
        }

        if (network_fatal_.exchange(false, std::memory_order_acq_rel)) {
            drain_inflight_with_error(MYMQ_Public::CommonErrorCode::NETWORK_FATAL);
        }
        std::cout << "[" << now_ms_time_gen_str() << "] Client I/O loop stopped." << std::endl;
    }
    bool handle_incoming_data() {
        while (true) {
            if (client_state.current_state == ClientState::READING_HEADER) {
                size_t bytes_read = 0;
                const size_t bytes_needed = HEADER_SIZE - client_state.received_bytes;
                int ret = SSL_read_ex(
                    ssl_,
                    client_state.header_buffer.data() + client_state.received_bytes,
                    bytes_needed,
                    &bytes_read
                );
                if (ret == 0) {
                    int err_code = SSL_get_error(ssl_, ret);
                    if (err_code == SSL_ERROR_WANT_READ || err_code == SSL_ERROR_WANT_WRITE) {
                        return true;
                    }
                    if (err_code == SSL_ERROR_ZERO_RETURN) {
                        cerr("The connection has been closed normally by the other party.");
                    } else if (err_code == SSL_ERROR_SYSCALL) {
                        std::cerr << "[" << now_ms_time_gen_str() << "] SSL Syscall error (Network broken)." << std::endl;
                    } else if (err_code == SSL_ERROR_SSL) {
                        std::cerr << "[" << now_ms_time_gen_str() << "] SSL Protocol error." << std::endl;
                        ERR_print_errors_fp(stderr);
                    } else {
                        cerr("UNKNOWN ERROR IN SSL_read_ex : " + std::to_string(err_code));
                    }
                    network_fatal_.store(true, std::memory_order_release);
                    running_.store(false);
                    return false;
                }
                client_state.received_bytes += bytes_read;
                if (client_state.received_bytes < HEADER_SIZE) {
                    continue;
                }

                uint32_t total_length_net;
                memcpy(&total_length_net, client_state.header_buffer.data(), sizeof(uint32_t));
                uint32_t total_length = ntohl(total_length_net);
                if (total_length < HEADER_SIZE) {
                    cerr("[" + now_ms_time_gen_str() + "] [Error] Malformed msg. Length: " + std::to_string(total_length));
                    network_fatal_.store(true, std::memory_order_release);
                    return false;
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
                if (client_state.expected_body_length == 0) {
                    handle_event(
                        client_state.event_type,
                        client_state.correlation_id,
                        client_state.ack_level,
                        MYMQ::OwnedBytes{}
                    );
                    client_state.reset(HEADER_SIZE);
                    continue;
                }

                MYMQ::Client::BufferPool::Block block;
                if (!MYMQ::Client::BufferPool::instance().try_allocate_for(
                        client_state.expected_body_length,
                        std::chrono::milliseconds(0),
                        block)) {
                    cerr("[" + now_ms_time_gen_str() + "] [Error] BufferPool exhausted while receiving response body.");
                    network_fatal_.store(true, std::memory_order_release);
                    running_.store(false);
                    return false;
                }
                auto* raw = static_cast<unsigned char*>(block.data);
                client_state.body_ptr = raw;
                client_state.body_owner = std::shared_ptr<void>(
                    raw,
                    [block = std::move(block)](void*) mutable {
                        MYMQ::Client::BufferPool::instance().release(block);
                    }
                );
                client_state.current_state = ClientState::READING_BODY;
                client_state.received_bytes = 0;
                continue;
            }

            size_t bytes_read = 0;
            const size_t bytes_needed = client_state.expected_body_length - client_state.received_bytes;
            int ret = SSL_read_ex(
                ssl_,
                client_state.body_ptr + client_state.received_bytes,
                bytes_needed,
                &bytes_read
            );
            if (ret == 0) {
                int err_code = SSL_get_error(ssl_, ret);
                if (err_code == SSL_ERROR_WANT_READ || err_code == SSL_ERROR_WANT_WRITE) {
                    return true;
                }
                if (err_code == SSL_ERROR_ZERO_RETURN) {
                    cerr("The connection has been closed normally by the other party.");
                } else if (err_code == SSL_ERROR_SYSCALL) {
                    std::cerr << "[" << now_ms_time_gen_str() << "] SSL Syscall error (Network broken)." << std::endl;
                } else if (err_code == SSL_ERROR_SSL) {
                    std::cerr << "[" << now_ms_time_gen_str() << "] SSL Protocol error." << std::endl;
                    ERR_print_errors_fp(stderr);
                } else {
                    cerr("UNKNOWN ERROR IN SSL_read_ex : " + std::to_string(err_code));
                }
                network_fatal_.store(true, std::memory_order_release);
                running_.store(false);
                return false;
            }
            client_state.received_bytes += bytes_read;
            if (client_state.received_bytes < client_state.expected_body_length) {
                continue;
            }

            if (!is_registered) {
                if (static_cast<Eve>(client_state.event_type) == MYMQ::EventType::SERVER_RESPONSE_REGISTER) {
                    MP mp(client_state.body_ptr, client_state.expected_body_length);
                    auto view = mp.read_bytes_view();
                    MP mp_content(view.first, view.second);
                    auto succ = mp_content.read_bool();
                    std::string resp = "Register result : '" + client_id_str + "' role(" + std::to_string(channel_role_) + ") register ";
                    if (succ) {
                        is_registered = 1;
                        resp += "success";
                    } else {
                        resp += "failed";
                    }
                    out(resp);
                } else {
                    std::cerr << "[" << now_ms_time_gen_str() << "] Not yet registered but received Event." << std::endl;
                }
                client_state.reset(HEADER_SIZE);
                continue;
            }

            MP mp(client_state.body_ptr, client_state.expected_body_length);
            auto view = mp.read_bytes_view();
            handle_event(
                client_state.event_type,
                client_state.correlation_id,
                client_state.ack_level,
                MYMQ::OwnedBytes{view.first, view.second, client_state.body_owner}
            );
            client_state.reset(HEADER_SIZE);
            continue;
        }
    }

    void handle_event(uint16_t eventtype, uint32_t correlation_id, uint16_t ack_level, MYMQ::OwnedBytes msg_body) {
        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;

        if (map_wait_responces.find(acc, correlation_id)) {
            curr_flying_request_num--;
            notify_send_state();
            auto cb = std::move(acc->second);
            map_wait_responces.erase(acc);
            cb(eventtype,std::move(msg_body) );
   }
        else {
            cerr("[" + now_ms_time_gen_str() + "] Received response for unknown or expired correlation_id: "
                +std:: to_string(correlation_id)+ " Event: " + MYMQ::to_string(static_cast<Eve>(eventtype)));

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
    size_t default_request_timeout_ms_ = MYMQ::REQUEST_TIMEOUT_MS_DEFAULT;


    SOCKET clientSocket;
    std::atomic<bool> running_{false};
    std::atomic<bool> network_fatal_{false};
    std::mutex send_state_mtx_;
    std::condition_variable send_state_cv_;
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

    MYMQ::ACK_Level ack_level=MYMQ::ACK_Level::ACK_PROMISE_INDISK;
    uint16_t channel_role_ = static_cast<uint16_t>(MYMQ_Public::ChannelRole::CONTROL);

    tbb::concurrent_hash_map<uint32_t, ResponseCallback> map_wait_responces;
    std::unique_ptr<RequestTimeoutQueue> request_timeout_timer;

    std::atomic<uint32_t> Correlation_ID{0};
    std::optional<size_t> request_timeout_ms_override_{};



    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;

    // 用于标记 SSL 握手是否完成
    std::atomic<bool> ssl_handshake_complete_{false};

    bool ssl_handshaked_ = false;    // 标记 SSL 握手是否完成
    bool ssl_want_read_ = false;     // 握手过程中 SSL 是否在等待读
    bool ssl_want_write_ = true;     // 握手过程中 SSL 是否在等待写 (初始为 true 以启动握手)

    std::atomic<size_t> curr_flying_request_num{0};
    std::vector<char> IO_buffer;
    size_t IO_buffer_size;


};

} // namespace Network
} // namespace MYMQ

#endif // COMMUNICATION_H
