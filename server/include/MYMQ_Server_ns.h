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
// ==========================================
// 1. ServerConsumerInfo
// ==========================================
struct ServerConsumerInfo {
    std::set<std::string> subscribed_topics;
    std::string memberid;
    std::string clientid;

    mutable std::mutex mtx;

    // 【1. 协议版本号】单调递增
    size_t generation_id;

    // 【2. 最终下发状态 (Last Sent Snapshot)】
    // 这是 Server 在上一轮心跳中发给 Client 的最终决定。
    // 如果 client_gen_id == generation_id，说明 Client 已经持有这个 Map。
    AssignmentMap assigned_partitions;

    // 【3. 悲观锁视图 (Pessimistic Lock View)】
    // 只有收到 Client 的 ACK (gen_id 匹配) 后，才会从这里移除分区。
    // 检查 "is_partition_in_use" 时，必须查这里！
    AssignmentMap current_holding;

    // 【4. 期望状态 (Ideal State)】
    // 由 Rebalance 算法计算得出，代表“如果世界完美，你应该拥有的分区”。
    AssignmentMap target_assignment;

    ServerConsumerInfo(std::set<std::string> topics, std::string memberid, std::string clientid_=MYMQ::clientid_DEFAULT)
        : subscribed_topics(topics), memberid(memberid), clientid(clientid_), generation_id(0) {}

    ServerConsumerInfo() : generation_id(0) {}
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
            uint32_t h = hash_func(v_node_key);
            auto it = ring.find(h);
            if (it != ring.end() && it->second == member_id) { //防hash冲突
                ring.erase(it);
            }
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
    mutable std::mutex group_mtx; // 保护整个 Group 状态

public:
    ConsumerGroupState(const std::string& id) : group_id(id) {}

private:
    // =================================================================
    // Private Helpers: _locked 后缀表示调用前必须持有 group_mtx
    // =================================================================

    // 检查全局锁：某个分区是否被任何成员（包括自己）的 current_holding 占用
    bool is_partition_in_use_locked(const std::string& topic, size_t p_id) {
        for (const auto& kv : members) {
            auto m = kv.second;
            auto it = m->current_holding.find(topic);
            if (it != m->current_holding.end()) {
                if (it->second.count(p_id)) return true;
            }
        }
        return false;
    }

    // 核心重平衡算法：只更新 target_assignment
    void perform_precise_rebalance_locked(MetadataCache& cache_ref) {
        // 1. 清空所有人的 Target
        for (auto& kv : members) {
            kv.second->target_assignment.clear();
        }

        // 2. 收集所有订阅的 Topic
        std::set<std::string> all_topics;
        for (const auto& kv : members) {
            all_topics.insert(kv.second->subscribed_topics.begin(),
                              kv.second->subscribed_topics.end());
        }

        // 3. 遍历 Topic -> Partition，查 Hash Ring 分配 Target
        for (const auto& topic : all_topics) {
            size_t partition_count = 0;
            if (!cache_ref.get_partition_count(topic, partition_count) || partition_count == 0)
                continue;

            for (size_t p_id = 0; p_id < partition_count; ++p_id) {
                // *** 整合点：传入 members 以供 HashRing 检查订阅关系 ***
                std::string owner_id = hash_ring.find_owner(topic, p_id, members);

                if (!owner_id.empty()) {
                    auto it = members.find(owner_id);
                    if (it != members.end()) {
                        it->second->target_assignment[topic].insert(p_id);
                    }
                }
            }
        }
    }

    // 核心状态机驱动：处理 ACK -> 对比 Target -> 生成 Next Assignment
    std::pair<size_t, AssignmentMap>
    handle_heartbeat_logic_locked(const std::string& member_id, size_t client_gen_id) {
        auto it = members.find(member_id);
        if (it == members.end()) return {0, {}};

        MemberPtr member = it->second;

        // --- Phase 1: 处理 ACK (释放锁) ---
        if (client_gen_id == member->generation_id) {
            member->current_holding = member->assigned_partitions;
        }

        // --- Phase 2: 计算 Next Step (Reconcile) ---
        AssignmentMap next_assignment;
        bool changes_needed = false;

        for (const auto& [topic, partitions] : member->target_assignment) {
            for (size_t pid : partitions) {

                bool am_i_holding = false;
                if (member->current_holding.count(topic) &&
                    member->current_holding.at(topic).count(pid)) {
                    am_i_holding = true;
                }

                if (am_i_holding) {
                    // Case A: Keep
                    next_assignment[topic].insert(pid);
                } else {
                    // Case B: Acquire
                    if (!is_partition_in_use_locked(topic, pid)) {
                        next_assignment[topic].insert(pid);
                        // Pre-claim
                        member->current_holding[topic].insert(pid);
                        changes_needed = true;
                    }
                    // Case C: Wait (implicitly)
                }
            }
        }

        // 检查隐式 Revoke (Assigned 有，但 Next 没有)
        if (next_assignment != member->assigned_partitions) {
            changes_needed = true;
        }

        // --- Phase 3: 推送变更 ---
        if (changes_needed) {
            member->generation_id++;
            member->assigned_partitions = next_assignment;
        }

        return {member->generation_id, member->assigned_partitions};
    }

public:
    // =================================================================
    // Public 接口
    // =================================================================

    std::pair<size_t, AssignmentMap>
    handle_heartbeat(const std::string& member_id, size_t client_gen_id) {
        std::lock_guard<std::mutex> lock(group_mtx);
        return handle_heartbeat_logic_locked(member_id, client_gen_id);
    }

    // 更新订阅 / 触发 Rebalance / 加入组
    std::pair<size_t, AssignmentMap>
    update_subscription(const std::string& member_id, size_t gen_id,
                        const std::set<std::string>& subs, MetadataCache& cache) {
        std::lock_guard<std::mutex> lock(group_mtx);

        auto it = members.find(member_id);
        if (it == members.end()) {
            auto new_ptr = std::make_shared<ServerConsumerInfo>(subs, member_id);
            members[member_id] = new_ptr;
            hash_ring.add_member(member_id);
            std::cout << "[Info] Member Joined: " << member_id << std::endl;
        } else {
            if (it->second->subscribed_topics != subs) {
                it->second->subscribed_topics = subs;
                std::cout << "[Info] Member Updated Subs: " << member_id << std::endl;
            }
        }

        perform_precise_rebalance_locked(cache);

        // 关键：复用逻辑，但不需要再次加锁
        return handle_heartbeat_logic_locked(member_id, gen_id);
    }

    bool handle_leave(const std::string& member_id, MetadataCache& cache_ref) {
        std::lock_guard<std::mutex> lock(group_mtx);
        if (members.erase(member_id)) {
            hash_ring.remove_member(member_id);
            if (!members.empty()) {
                perform_precise_rebalance_locked(cache_ref);
            }
            std::cout << "[Info] Member Left: " << member_id << std::endl;
            return true;
        }
        return false;
    }
};

}


#endif // MYMQ_SERVER_NS_H
