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
#include"../src/Controller.h"
#include"MYMQ_innercodes.h"
#include"../src/MurmurHash2.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include"tbb/concurrent_hash_map.h"
#include "uuid/uuid.h"

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

using AssignmentMap = std::map<std::string, std::set<size_t>>;
struct ServerConsumerInfo{
    std::set<std::string> subscribed_topics;

        // 实际分配到的分区 (Topic -> Set of PartitionIDs)
        // 这种结构方便做 diff，也方便快速查询 "我拥有Topic A的哪些分区？"
        std::map<std::string, std::set<size_t>> assigned_partitions;

        std::string memberid;
        size_t generation_id;
        std::string clientid;
        mutable std::mutex mtx; // 保护 assigned_partitions 和 generation_id

    ServerConsumerInfo(std::set<std::string> topics,std::string memberid,std::string clientid_=MYMQ::clientid_DEFAULT)
        :subscribed_topics(topics),memberid(memberid),generation_id(0),clientid(clientid_){}
    ServerConsumerInfo():subscribed_topics(std::set<std::string>()),memberid(std::string()),generation_id(0),clientid(MYMQ::clientid_DEFAULT){}
};


class ConsistentHashRing {
private:
    // Hash值 -> 实际 MemberID
    std::map<uint32_t, std::string> ring;
    int virtual_node_count = 100; // 每个成员的虚拟节点数

    // 简单的 MurmurHash2 或类似算法 (此处简化用 std::hash，实际建议用 Murmur)
    uint32_t hash_func(const std::string& key) {

        return MurmurHash2::calculate_32(key.data(),key.size(),0x9747b28c);
    }

public:
    void add_member(const std::string& member_id) {
        for (int i = 0; i < virtual_node_count; ++i) {
            std::string v_node_key = member_id + "#" + std::to_string(i);
            ring[hash_func(v_node_key)] = member_id;
        }
    }

    void remove_member(const std::string& member_id) {
        for (int i = 0; i < virtual_node_count; ++i) {
            std::string v_node_key = member_id + "#" + std::to_string(i);
            ring.erase(hash_func(v_node_key));
        }
    }

    // 核心：给定一个资源（Topic + Partition），找到负责它的 Member
    // 需要传入 members map 来检查该 Member 是否真的订阅了这个 Topic
    std::string find_owner(const std::string& topic, int partition_id,
                           const std::map<std::string, std::shared_ptr<ServerConsumerInfo>>& members_ref) {
        if (ring.empty()) return "";

        std::string resource_key = topic + "-" + std::to_string(partition_id);
        uint32_t h = hash_func(resource_key);

        // 在环上找到第一个 hash 值 >= h 的节点
        auto it = ring.lower_bound(h);

        // 环形查找：我们需要遍历环，直到找到一个 *订阅了该 Topic* 的 Member
        // 如果转了一圈都没人订阅，说明这个分区没人能接管
        auto start_it = it;
        if (it == ring.end()) it = ring.begin();

        // 防止死循环的最大迭代次数 (所有虚拟节点数)
        size_t max_steps = ring.size();
        size_t steps = 0;

        while (steps < max_steps) {
            std::string candidate_id = it->second;

            // 检查 candidate 是否存在且订阅了该 Topic
            // 注意：这里需要外部保证 members_ref 的线程安全或只读访问
            auto mem_it = members_ref.find(candidate_id);
            if (mem_it != members_ref.end()) {
                // *** 关键约束 ***: 一致性 Hash 选出来的节点，必须订阅了该 Topic
                if (mem_it->second->subscribed_topics.count(topic)) {
                    return candidate_id;
                }
            }

            // 顺时针找下一个
            ++it;
            if (it == ring.end()) it = ring.begin();
            steps++;
        }

        return ""; // 无人订阅该 Topic
    }
};


class ConsumerGroupState {
public:
    using MemberPtr = std::shared_ptr<ServerConsumerInfo>;
    using MemberMap = std::map<std::string, MemberPtr>;

private:
    std::string group_id;
    MemberMap members;
    ConsistentHashRing hash_ring;
    mutable std::mutex group_mtx;

public:
    ConsumerGroupState(const std::string& id) : group_id(id) {}


    std::pair<size_t, AssignmentMap>
        handle_heartbeat(const std::string& member_id, size_t client_gen_id) {

            std::lock_guard<std::mutex> lock(group_mtx);

            auto it = members.find(member_id);
            if (it == members.end()) {
                // 异常情况：该成员可能因为超时被踢出了，需要重新 Join
                return {0, {}};
            }

            MemberPtr member = it->second;

            // --- 核心 Check Diff ---
            if (client_gen_id != member->generation_id) {
                // 客户端版本落后了！说明在上次心跳间隔内，发生了重平衡（比如 B 加入了）
                // Server 直接把已经算好的结果返回去
                return {member->generation_id, member->assigned_partitions};
            }
            else {
                // 版本一致，无事发生
                // 返回空 Map，表示 "Keep Existing Assignment"
                return {client_gen_id, {}};
            }
        }
    // =================================================================
    // 核心接口：更新订阅 + 立即获取结果
    // 返回值: pair<GenerationID, AssignmentMap>
    // =================================================================
    std::pair<size_t, AssignmentMap>
    update_subscription(const std::string& member_id,
                        const std::set<std::string>& client_full_list,
                        MetadataCache& cache_ref) {

        std::lock_guard<std::mutex> lock(group_mtx);

        auto it = members.find(member_id);
        bool need_rebalance = false;

        // 1. 只有订阅列表真的变了，或者新成员加入，才标记需要重平衡
        if (it == members.end()) {
            auto new_member = std::make_shared<ServerConsumerInfo>(client_full_list,member_id);

            members[member_id] = new_member;
            hash_ring.add_member(member_id);
            need_rebalance = true;
        } else {
            if (it->second->subscribed_topics != client_full_list) {
                it->second->subscribed_topics = client_full_list;
                need_rebalance = true;
            }
        }

        // 2. 如果需要重平衡，执行并更新受影响的人
        if (need_rebalance) {
            perform_precise_rebalance(cache_ref);
        }

        // 3. 无论是否重平衡，都立即返回该成员当前的最新状态
        // 客户端拿到这个直接覆盖本地，无需等待心跳
        auto member = members[member_id];
        return {member->generation_id, member->assigned_partitions};
    }

    // 成员离开
    bool handle_leave(const std::string& member_id, MetadataCache& cache_ref) {
        std::lock_guard<std::mutex> lock(group_mtx);
        if (members.erase(member_id)) {
            hash_ring.remove_member(member_id);
            if (!members.empty()) {
                perform_precise_rebalance(cache_ref);
                return 1;
            }
        }
        return 0;
    }

private:
    // =================================================================
    // 核心逻辑：精准重平衡 (Precise Rebalance)
    // =================================================================
    void perform_precise_rebalance(MetadataCache& cache_ref) {
        // 1. 预计算阶段：建立一个临时的 Map 存放计算结果
        //    key: member_id, value: 新的分配方案
        std::map<std::string, AssignmentMap> proposals;

        // 初始化：为每个存在的成员建立空条目
        std::set<std::string> all_interested_topics;
        for (const auto& kv : members) {
            proposals[kv.first] = {}; // 先置空
            all_interested_topics.insert(kv.second->subscribed_topics.begin(),
                                         kv.second->subscribed_topics.end());
        }

        // 2. 计算阶段：遍历所有 Topic 分区，在环上找主人
        for (const auto& topic : all_interested_topics) {
            size_t partition_count=0 ;
            bool succ= cache_ref.get_partition_count(topic,partition_count);
            if (!succ || partition_count == 0) continue; // 获取失败直接跳过
            for (int p_id = 0; p_id < partition_count; ++p_id) {
                // 在环上找到归属的 MemberID
                std::string owner_id = hash_ring.find_owner(topic, p_id, members);
                if (!owner_id.empty()) {
                    // 记录到临时方案中
                    proposals[owner_id][topic].insert(p_id);
                }
            }
        }

        // 3. 应用阶段：Diff 比对
        //    只修改真正发生变化的成员
        for (auto& kv : members) {
            const std::string& m_id = kv.first;
            MemberPtr member = kv.second;

            // 取出该成员的新计算结果
            AssignmentMap& new_assignment = proposals[m_id];

            // *** 关键 ***：C++ Map 的 operator== 会深度比较内容
            if (member->assigned_partitions != new_assignment) {
                // 只有真的变了，才更新状态 + 递增世代
                member->assigned_partitions = std::move(new_assignment);
                member->generation_id++;

                // log: Member [m_id] assignment changed to Gen [id]
            } else {
                // 没变，什么都不做！GenID 保持不变！
                // log: Member [m_id] assignment stable.
            }
        }
    }
};


}


#endif // MYMQ_SERVER_NS_H
