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
using Err=MYMQ_Public::CommonErrorCode;
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

// Removed SendFileTask, ClientState, TcpSession as they are now provided by generic Net namespace in Server.h
// or are no longer needed in this namespace.

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
    using TopicPartition = MYMQ_Public::TopicPartition; // 使用全局定义
    struct OffsetAndMetadata{
        size_t offset=0;
        size_t genid=0;
    };

private:
    std::string group_id;
    MemberMap members;
    ConsistentHashRing hash_ring;
    mutable std::mutex group_mtx;

    // 【新增】反向索引：Topic -> Partition -> MemberID
    // 用于 O(1) 查找某个分区当前被谁持有着（基于 current_holding）
    std::map<std::string, std::map<size_t, std::string>> partition_owners_;
    std::map<TopicPartition,OffsetAndMetadata> map_OffsetAndMetadata;

    // 【新增】撤销回调
    using RevocationCallback = std::function<void(const std::string&, size_t)>;
    RevocationCallback revocation_cb_;

public:
    ConsumerGroupState(const std::string& id) : group_id(id) {}

    void set_revocation_callback(RevocationCallback cb) {
        std::lock_guard<std::mutex> lock(group_mtx);
        revocation_cb_ = cb;
    }

    // 【新增】公共查询接口（调试/管理用）
    std::string get_partition_owner(const std::string& topic, size_t partition) {
        std::lock_guard<std::mutex> lock(group_mtx);
        if (partition_owners_.count(topic) && partition_owners_[topic].count(partition)) {
            return partition_owners_[topic][partition];
        }
        return "";
    }

private:
    // =================================================================
    // Private Helpers: 维护反向索引的工具函数
    // =================================================================

    // 辅助：从反向表中移除某人持有的特定分区
    void remove_ownership_index(const std::string& member_id, const std::string& topic, size_t pid) {
        auto& p_map = partition_owners_[topic];
        auto it = p_map.find(pid);
        if (it != p_map.end() && it->second == member_id) {
            p_map.erase(it);
            
            // 【关键】通知外部：分区所有权已撤销
            // 这将触发 Long Polling 提前返回
            if (revocation_cb_) {
                revocation_cb_(topic, pid);
            }

            // 如果该 Topic 下没有分区了，可以清理 Topic key
            if (p_map.empty()) partition_owners_.erase(topic);
        }
    }

    // 辅助：在反向表中注册归属权
    void add_ownership_index(const std::string& member_id, const std::string& topic, size_t pid) {
        partition_owners_[topic][pid] = member_id;
    }

    // 辅助：完全清理某人的所有反向索引 (用于 Leave 或 ACK 重置时)
    void clear_member_from_index(const std::string& member_id, const AssignmentMap& holding_to_clear) {
        for (const auto& [topic, parts] : holding_to_clear) {
            for (size_t pid : parts) {
                remove_ownership_index(member_id, topic, pid);
            }
        }
    }

    // =================================================================
    // 逻辑核心区
    // =================================================================

    // 【修改】检查全局锁：直接查反向表，不再遍历 Members
    // 复杂度：O(log P) vs 原来的 O(M * log P)
    bool is_partition_in_use_locked(const std::string& topic, size_t p_id) {
        auto t_it = partition_owners_.find(topic);
        if (t_it == partition_owners_.end()) return false;
        return t_it->second.count(p_id) > 0;
    }

    // 核心重平衡算法：只更新 target_assignment
    // 【整合】使用旧版的完整逻辑
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
            // 从缓存获取分区数
            if (!cache_ref.get_partition_count(topic, partition_count) || partition_count == 0)
                continue;

            for (size_t p_id = 0; p_id < partition_count; ++p_id) {
                // HashRing 检查订阅关系并分配
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

    // 【修改】核心状态机：在修改 current_holding 时同步更新 partition_owners_
    std::pair<size_t, AssignmentMap>
    handle_heartbeat_logic_locked(const std::string& member_id, size_t client_gen_id) {
        auto it = members.find(member_id);
        if (it == members.end()) return {0, {}};

        MemberPtr member = it->second;

        // --- Phase 1: 处理 ACK (释放锁) ---
        if (client_gen_id == member->generation_id) {
            // [Sync Point 1]
            // 客户端确认了 assigned_partitions。这意味着 member->current_holding 将变为 assigned_partitions。
            // 我们需要维护反向索引：

            // 只有当 current_holding 确实发生变化时才操作，避免无意义的开销
            if (member->current_holding != member->assigned_partitions) {
                // 1. 从反向表中移除旧的 current_holding
                clear_member_from_index(member_id, member->current_holding);

                // 2. 更新 holding
                member->current_holding = member->assigned_partitions;

                // 3. 将新的 assigned_partitions 加入反向表
                for (const auto& [topic, parts] : member->current_holding) {
                    for (size_t pid : parts) {
                        add_ownership_index(member_id, topic, pid);
                    }
                }
            }
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
                    // [Optimization] 这里调用的是优化后的 O(1) 检查
                    if (!is_partition_in_use_locked(topic, pid)) {
                        next_assignment[topic].insert(pid);

                        // [Sync Point 2] Pre-claim (预占)
                        // 在写入 current_holding 的同时，写入反向索引
                        member->current_holding[topic].insert(pid);
                        add_ownership_index(member_id, topic, pid);

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

        // 重新计算所有人的 Target
        perform_precise_rebalance_locked(cache);

        // 计算并返回当前成员的分配结果
        return handle_heartbeat_logic_locked(member_id, gen_id);
    }

    // 【修改】处理成员离开：需要清理反向索引
    bool handle_leave(const std::string& member_id, MetadataCache& cache_ref) {
        std::lock_guard<std::mutex> lock(group_mtx);
        auto it = members.find(member_id);
        if (it != members.end()) {
            // [Sync Point 3] Member Leaving
            // 务必在删除 member 之前清理反向索引
            clear_member_from_index(member_id, it->second->current_holding);

            members.erase(it);
            hash_ring.remove_member(member_id);

            if (!members.empty()) {
                perform_precise_rebalance_locked(cache_ref);
            }
            std::cout << "[Info] Member Left and Index Cleared: " << member_id << std::endl;
            return true;
        }
        return false;
    }



    Err commit_offset(const std::string& member_id, size_t client_gen_id,
                           const std::string& topic, size_t partition, size_t offset) {
            std::lock_guard<std::mutex> lock(group_mtx);

            // 1. 验证成员是否存在
            auto it = members.find(member_id);
            if (it == members.end()) {
                return Err::MEMBER_NOT_FOUND; // 成员不在组内
            }
            MemberPtr member = it->second;

            // 2. Fencing 验证 (关键): 检查 Generation ID
            // 如果客户端的代数小于服务端记录的代数，说明发生了重平衡，该请求是过期的
            if (client_gen_id != member->generation_id) {
                std::cout << "[Warning] Commit rejected: Stale generation. Client: "
                          << client_gen_id << " Server: " << member->generation_id << std::endl;
                return Err::GENERATION_EXPIRED;
            }

            // 3. 所有权验证 (关键): 确保成员当前确实持有该分区
            // 注意：必须查 current_holding (客户端已确认持有的)，而不是 assigned (服务端预想分配的)
            bool is_holding = false;
            if (member->current_holding.count(topic) &&
                member->current_holding.at(topic).count(partition)) {
                is_holding = true;
            }

            if (!is_holding) {
                std::cout << "[Warning] Commit rejected: Member " << member_id
                          << " does not own " << topic << "-" << partition << std::endl;
                return Err::GENERATION_EXPIRED;
            }

            // 4. 写入 Offset 存储
            // 假设 TopicPartition 结构体支持聚合初始化 {topic, partition}
            TopicPartition tp{topic, partition};
            map_OffsetAndMetadata[tp] = {offset, client_gen_id};

            return Err::NULL_ERROR;
        }


        Err commit_offsets(const std::string& member_id, size_t client_gen_id,
                            const std::map<std::string, std::map<size_t, size_t>>& offsets) {
            std::lock_guard<std::mutex> lock(group_mtx);

            auto it = members.find(member_id);
            if (it == members.end()) return Err::MEMBER_NOT_FOUND;
            MemberPtr member = it->second;

            // Fencing 验证
            if (client_gen_id != member->generation_id) return Err::GENERATION_EXPIRED;

            bool all_success = true;

            for (const auto& [topic, part_map] : offsets) {
                // 检查是否持有该 Topic (快速失败检查)
                if (member->current_holding.find(topic) == member->current_holding.end()) {
                    all_success = false;
                    continue; // 跳过整个 Topic
                }

                const auto& holding_parts = member->current_holding.at(topic);

                for (const auto& [pid, offset] : part_map) {
                    // 检查具体分区所有权
                    if (holding_parts.count(pid)) {
                        TopicPartition tp{topic, pid};
                        map_OffsetAndMetadata[tp] = {offset, client_gen_id};
                    } else {
                        all_success = false; // 只要有一个非法，就标记（也可以选择在这里中断）
                    }
                }
            }
            if(!all_success){
                return Err::GENERATION_EXPIRED;
            }
            return Err::NULL_ERROR;
        }

        bool get_committed_offset(const std::string& topic, size_t partition,size_t& offset_ref) {
            std::lock_guard<std::mutex> lock(group_mtx);
            TopicPartition tp{topic, partition};

            auto it = map_OffsetAndMetadata.find(tp);
            if (it != map_OffsetAndMetadata.end()) {
               offset_ref= it->second.offset;
                return 1;
            }
            return 0; // 表示该分区从未提交过 Offset
        }









};


}


#endif // MYMQ_SERVER_NS_H
