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

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);

        if (inet_pton(AF_INET, server_IP.c_str(), &serverAddr.sin_addr) != 1) {
            closesocket(clientSocket);
            WSACleanup();
            throw std::runtime_error("Invalid IP address or inet_pton failed for IP: " + server_IP);
        }

        iResult = connect(clientSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
        if (iResult == SOCKET_ERROR) {
            int lastError = WSAGetLastError();
            if (lastError == WSAEWOULDBLOCK || lastError == WSAEINPROGRESS) {
                connect_pending_.store(true);
            } else {
                closesocket(clientSocket);
                WSACleanup();
                throw std::runtime_error("Connect failed with error: " + std::to_string(lastError));
            }
        } else {
            std::cout << "Connection with server built" << std::endl;
            connection_established_.store(true);
            connect_pending_.store(false);
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
        if (!connection_established_.load()) {
            send_pending.store(false);
            return;
        }

        // 1. 优先发送 client_id 消息
        if (client_id_message_to_send_.has_value()) {
            auto& pair = client_id_message_to_send_.value();
            auto& msg = pair.first;
            auto& off = pair.second;

            const char* byte_to_send = reinterpret_cast<const char*>(msg.data() + off);
            size_t bytes_remaining = msg.size() - off;

            int res = send(clientSocket, byte_to_send, bytes_remaining, 0);

            if (res == SOCKET_ERROR) {
                int lastError = WSAGetLastError();
                if (lastError == WSAEWOULDBLOCK) {
                    return;
                } else {
                    std::cerr << "[" << now_ms_time_gen_str() << "] send failed with error for client_id: " << lastError << std::endl;
                    client_id_message_to_send_.reset();
                    running_.store(false);
                    return;
                }
            } else if (res == 0) {
                std::cerr << "[" << now_ms_time_gen_str() << "] Connection closed by peer during client_id send." << std::endl;
                client_id_message_to_send_.reset();
                running_.store(false);
                return;
            } else {
                off += res;
                if (off == msg.size()) {
                    std::cout << "[" << now_ms_time_gen_str() << "] Client ID message fully sent." << std::endl;
                    client_id_message_to_send_.reset();
                }
            }
        }

        if (client_id_message_to_send_.has_value()) {
            send_pending.store(true);
            return;
        }

        if (!is_registered.load()) {
            send_pending.store(send_queue.peek() != nullptr);
            return;
        }

        while (true) {
            PendingMessage* current_msg_ptr = send_queue.peek(); // 改为 PendingMessage*
            if (current_msg_ptr == nullptr) break;

            auto& msg = current_msg_ptr->message_bytes;
            auto& off = current_msg_ptr->offset;

            const char* byte_to_send = reinterpret_cast<const char*>(msg.data() + off);
            size_t bytes_remaining = msg.size() - off;

            int res = send(clientSocket, byte_to_send, bytes_remaining, 0);

            if (res == SOCKET_ERROR) {
                int lastError = WSAGetLastError();
                if (lastError == WSAEWOULDBLOCK) {
                    break;
                } else {
                    std::cerr << "[" << now_ms_time_gen_str() << "] send failed with error: " << lastError << std::endl;
                    PendingMessage dummy;
                    send_queue.try_dequeue(dummy);
                    running_.store(false);
                    return;
                }
            } else if (res == 0) {
                std::cerr << "[" << now_ms_time_gen_str() << "] Connection closed by peer during send (returned 0 bytes)." << std::endl;
                PendingMessage dummy;
                send_queue.try_dequeue(dummy);
                running_.store(false);
                return;
            } else {
                off += res;
                if (off == msg.size()) {
                    tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
                    map_wait_responces.insert(acc, current_msg_ptr->coid);
                    acc->second = std::move(current_msg_ptr->handler);
                    acc.release();

                    // 2. 启动定时器
                    auto coid=current_msg_ptr->coid;
                    timer.commit_s([this, coid]{ // 捕获 coid
                        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc_timer;
                        if( map_wait_responces.find(acc_timer, coid)){
                            cerr("[Request Time out] Correlationid : "+std::to_string(coid));
                            map_wait_responces.erase(acc_timer);
                        }
                    }, request_timeout_s, request_timeout_s, 1);

                    // 3. 将消息出队
                    PendingMessage dummy;
                    send_queue.try_dequeue(dummy);
                }
                else {
                    break;
                }
            }
        }

        send_pending.store(client_id_message_to_send_.has_value() || send_queue.peek() != nullptr);
    }

    // (logic unchanged)
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

    // (logic unchanged)
    void io_loop() {
        FD_SET read_fds, write_fds;
        timeval timeout;

        while (running_) {
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);

            if (connection_established_.load()) {
                FD_SET(clientSocket, &read_fds);
            }

            if (send_pending.load() || connect_pending_.load()) {
                FD_SET(clientSocket, &write_fds);
            }

            timeout.tv_sec = 0;
            timeout.tv_usec = 10000; // 10ms

            int result = select(0, &read_fds, &write_fds, nullptr, &timeout);

            if (result == SOCKET_ERROR) {
                int lastError = WSAGetLastError();
                if (lastError == WSAEINTR) continue;
                std::cerr << "[" << now_ms_time_gen_str() << "] select() failed with error: " << lastError << std::endl;
                running_.store(false);
                break;
            }

            if (result == 0) continue;

            if (connect_pending_.load() && FD_ISSET(clientSocket, &write_fds)) {
                int opt_val;
                int opt_len = sizeof(opt_val);
                if (getsockopt(clientSocket, SOL_SOCKET, SO_ERROR, (char*)&opt_val, &opt_len) == SOCKET_ERROR) {
                    std::cerr << "[" << now_ms_time_gen_str() << "] getsockopt(SO_ERROR) failed: " << WSAGetLastError() << std::endl;
                    running_.store(false);
                    break;
                }

                if (opt_val == 0) {
                    std::cout << "[" << now_ms_time_gen_str() << "] Non-blocking connect successful." << std::endl;
                    connection_established_.store(true);
                    connect_pending_.store(false);
                } else {
                    std::cerr << "[" << now_ms_time_gen_str() << "] Non-blocking connect failed with error: " << opt_val << std::endl;
                    running_.store(false);
                    break;
                }
            }

            if (connection_established_.load() && !client_id_sent_.load()) {
                send_client_id_on_connect();
                attemped_send();
            }
            else if (connection_established_.load() && FD_ISSET(clientSocket, &write_fds) && send_pending.load()) {
                attemped_send();
            }

            if (connection_established_.load() && FD_ISSET(clientSocket, &read_fds)) {
                if (!handle_incoming_data(static_cast<int>(clientSocket))) {
                    running_.store(false);
                    break;
                }
            }
        }
        std::cout << "[" << now_ms_time_gen_str() << "] Client I/O loop stopped." << std::endl;
    }


    bool handle_incoming_data(int sock) {
        char buffer[4096];
        int bytes_received = recv(sock, buffer, sizeof(buffer), 0);

        if (bytes_received == SOCKET_ERROR) {
            int lastError = WSAGetLastError();
            if (lastError == WSAEWOULDBLOCK) return true; // 非阻塞，正常返回
            std::cerr << "[" << now_ms_time_gen_str() << "] Error receiving data: " << lastError << std::endl;
            return false; // 真正发生错误
        }
        if (bytes_received == 0) {
            std::cout << "[" << now_ms_time_gen_str() << "] Server closed connection gracefully." << std::endl;
            return false; // 连接已关闭
        }

        // 跟踪我们在 buffer 中处理了多少字节
        int bytes_processed = 0;

        // 只要 buffer 中还有未处理的数据，就持续循环
        // 这可以正确处理“粘包”（一个 buffer 里有多条消息）
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
                    // --- 你的头部解析逻辑（完全不变） ---
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
                        handle_event(client_state.event_type, client_state.correlation_id, client_state.ack_level, Mybyte{}, sock);
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
                    // --- 你的 body 处理逻辑（完全不变） ---
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
                                handle_event(client_state.event_type, client_state.correlation_id, client_state.ack_level, std::move(client_state.body_buffer), sock);
                            } else {
                                MP mp(client_state.body_buffer);
                                handle_event(client_state.event_type, client_state.correlation_id, client_state.ack_level, std::move(mp.read_uchar_vector()), sock);
                            }
                        }
                    }
                    client_state.reset(HEADER_SIZE);
                }
            }
        }

        return true;
    }

    void handle_event(uint16_t eventtype, uint32_t correlation_id, uint16_t ack_level, Mybyte msg_body, int client_fd) {
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

};

#endif // COMMUNICATION_H
