#ifndef MYMQ_SERVER_NS_H
#define MYMQ_SERVER_NS_H

#include <vector>
#include <string>
#include <unordered_map>
#include <set>
#include <atomic>
#include <cstddef>
#include <sys/types.h>
#include <utility>
#include"MYMQ_innercodes.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace MYMQ_Server {




struct MessageLocation {
    int     file_descriptor=-1; // 日志文件的 fd
    off_t   offset_in_file=0;  // 在文件内的物理偏移
    size_t  length=0;          // 消息长度
    bool    found=0;
    size_t offset_next_to_consume=0;
};


class ExpectedMemberList {
public:

    ExpectedMemberList(const std::vector<std::string> & initial_consumers) {
        reset(initial_consumers);
    }

    void reset(const std::vector<std::string> & new_consumers) {
        people_status_.clear();
        uncalled_count_.store( 0);
        for(const auto& name:new_consumers){
            people_status_.emplace(name, false);
            uncalled_count_.fetch_add(1);
        }


    }


    bool callandcheck(const std::string& name) {
        auto it = people_status_.find(name);
        if (it != people_status_.end()) {
            if (!it->second) {
                it->second = true;
                uncalled_count_.fetch_sub(1);

            }

        }
        return areAllCalled();
    }



private:
    bool areAllCalled() const {
        return uncalled_count_.load() <= 0;
    }
private:
    std::unordered_map<std::string, bool> people_status_;
    std::atomic<size_t>  uncalled_count_ = 0;
};

struct SendFileTask {
    int in_fd;           // 文件描述符
    off_t offset;        // 初始偏移量
    size_t length;       // 发送长度
    size_t sent_so_far;  // 已发送字节数


     bool header_sent=0;
     std::vector<unsigned char>   header_data;
     size_t header_send_offset=0;
    // 下面这些仅作记录用，如果不需要可以删掉
    size_t offset_next_to_consume;
    std::string topicname;
    size_t partition;
    uint32_t correlation_id;
    uint16_t ack_level;

    SendFileTask(int fd, off_t off, size_t len, size_t first_off, const std::string& topic, size_t par, uint32_t cid, uint16_t ack)
        : in_fd(fd), offset(off), length(len), sent_so_far(0),
          offset_next_to_consume(first_off), topicname(topic), partition(par), correlation_id(cid), ack_level(ack) {}
    SendFileTask(MYMQ_Server::MessageLocation mesloc, const std::string& topic, size_t par, uint32_t cid, uint16_t ack)
        : in_fd(mesloc.file_descriptor), offset(mesloc.offset_in_file), length(mesloc.length), sent_so_far(0),
          offset_next_to_consume(mesloc.offset_next_to_consume), topicname(topic), partition(par),correlation_id(cid),ack_level(ack) {}
};

class ClientState {
    public:
    enum State {
        READING_HEADER,
        READING_BODY
    };

    // 状态管理
    State current_state = READING_HEADER;

    // 接收缓冲区
    std::vector<unsigned char> header_buffer;
    size_t bytes_read_in_header = 0;

    std::vector<unsigned char> body_buffer;
    size_t bytes_read_in_body = 0;

    // 协议解析字段
    uint32_t expected_body_length = 0;
    uint16_t event_type = 0;
    uint32_t correlation_id = 0;
    uint16_t ack_level = 0;

    std::atomic<bool> is_closing{false};
    int fd = -1;

    bool id_registered = false;
    std::string clientid = "UNKNOWN";

    SSL* ssl = nullptr;
    bool is_handshake_complete = false;
    bool enable_sendfile = false;

    uint32_t last_events=UINT32_MAX;

    // --- 发送相关结构 ---

    // 文件发送任务 (精简版：只存文件元数据，不存 Header)


    // 发送队列元素：可以是普通字节(Mybyte) 或 文件任务
    using SendItem = std::variant<std::vector<unsigned char>, SendFileTask>;

    std::deque<SendItem> send_queue;
    size_t current_vec_send_offset = 0; // 如果队首是 vector，记录发送到了哪里
    bool is_writing = false;
    std::mutex send_queue_mtx; // 专门锁队列的锁

    // 构造函数
    ClientState(size_t header_size) : header_buffer(header_size) {}
    ClientState() = default;


    bool is_closed(){
    return  this->is_closing.load();
        }
    int get_fd(){
        return this->fd;
    }

    // --- 核心入队函数 ---
    // 统一处理 ResponsePayload (可能是字节，也可能是文件)
    void enqueue_message(uint16_t event_type, uint32_t correlation_id, uint16_t ack_level,  std::variant<std::vector<unsigned char>, SendFileTask> payload) {

        short event_type_s = static_cast<short>(event_type);

        // -------------------------------------------------------
        // 情况 A: 发送普通字节消息
        // -------------------------------------------------------
        if (std::holds_alternative<std::vector<unsigned char>>(payload)) {
            auto& msg_body = std::get<std::vector<unsigned char>>(payload);

            MessageBuilder mb;
            uint32_t total_length = static_cast<uint32_t>(MYMQ::HEADER_SIZE + sizeof(uint32_t) + msg_body.size());

            mb.reserve(total_length);
            mb.append_uint32(total_length);
            mb.append_uint16(event_type_s);
            mb.append_uint32(correlation_id);
            mb.append_uint16(ack_level);
            mb.append_uchar_vector(msg_body); // 拷贝 body

            std::vector<unsigned char> full_message = std::move(mb.data);

            {
                std::unique_lock<std::mutex> statelock(this->send_queue_mtx);
                this->send_queue.emplace_back(std::move(full_message));
                if (!this->send_queue.empty()) this->is_writing = true;
            }
        }
        // -------------------------------------------------------
        // 情况 B: 发送文件 (Zero-Copy)
        // -------------------------------------------------------
        else if (std::holds_alternative<SendFileTask>(payload)) {
            auto& file_task = std::get<SendFileTask>(payload);

            {
                std::unique_lock<std::mutex> statelock(this->send_queue_mtx);

                // 再放文件任务
                this->send_queue.emplace_back(std::move(file_task));

                if (!this->send_queue.empty()) this->is_writing = true;
            }
        }
    }

};

class TcpSession {
public:
    TcpSession(std::shared_ptr<ClientState> state) : state_(state) {clientid=state->clientid;}

    void send(MYMQ::EventType type, uint32_t cid, uint16_t ack, std::variant<std::vector<unsigned char>, SendFileTask> payload) {
            auto state = state_.lock();
            if (!state || state->is_closed()) return;
            state->enqueue_message(static_cast<uint16_t>( type), cid, ack, std::move(payload));
        }
    int fd() const {
        auto state = state_.lock();
        return state ? state->get_fd() : -1;
    }

    // 检查连接是否有效
    bool is_connected() const {
        auto state = state_.lock();
        return state && !state->is_closed();
    }
    std::string get_clientid(){
        return clientid;
    }

private:
    std::weak_ptr<ClientState> state_;
    std::string clientid;
};


struct ServerConsumerInfo{
    std::set<std::string> subscribed_topics;
    std::string memberid;
    int generation_id;
    TcpSession session;
    uint32_t correlation_id_lastjoin;
    std::string clientid;
    ServerConsumerInfo(std::set<std::string> topics,std::string memberid,int generation_id,uint32_t correlation_id_lastjoin,TcpSession session_,std::string clientid_=MYMQ::clientid_DEFAULT)
        :subscribed_topics(topics),memberid(memberid),generation_id(generation_id),session(session_),correlation_id_lastjoin(correlation_id_lastjoin),clientid(clientid_){}
    ServerConsumerInfo():subscribed_topics(std::set<std::string>()),memberid(std::string()),generation_id(-2),clientid(MYMQ::clientid_DEFAULT),correlation_id_lastjoin(0),session(nullptr){}
};

struct ConsumerGroupState {
    std::string group_id;
    std::map<std::string, ServerConsumerInfo> members; // member_id -> ConsumerInfo
    MYMQ_Server:: ExpectedMemberList  expected_members;
    std::map<std::string, std::chrono::steady_clock::time_point> last_heartbeat; // member_id -> 最后心跳时间
    std::map<std::string, std::map<std::string, std::set<size_t>>> assignments; // member_id -> topic -> 分配的分区ID集合
    int generation_id; // 组的世代ID，每次再平衡后递增
    std::map<std::string,std::set<std::string>> map_subscribed_topics; // 组内所有消费者订阅的主题映射
    std::string leader_id;
    size_t rebalance_timeout_taskid=0;
    size_t join_collect_timeout_taskid=0;

    // 状态：

    enum GroupState { STABLE, JOIN_COLLECTING, AWAITING_SYNC,EMPTY};
    GroupState state;
    std::mutex state_mutex; // 保护组状态的互斥锁
    std::condition_variable rebalance_cv; // 用于 JoinGroup 阶段等待再平衡准备完成
    std::condition_variable sync_cv; // 新增：用于 SyncGroup 阶段等待分配结果

    ConsumerGroupState(const std::string& id)
        : group_id(id),
        expected_members(std::vector<std::string>{}),
        state(EMPTY),
        generation_id(-1)

    {
        // 可以在这里进行其他初始化
    }
    std::atomic<bool> rebalance_should = false;
    std::atomic<bool> rebalance_ing = false;




};

}


#endif // MYMQ_SERVER_NS_H
