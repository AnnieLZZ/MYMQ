#ifndef CONTROLLER_H
#define CONTROLLER_H
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
constexpr size_t LOCAL_BROKER_ID=0;
// 简单的 Broker 信息（单机其实只需要存一份全局配置即可）
struct BrokerNode {
    size_t id;
    std::string host;
    int port;
};

struct PartitionMetadata {
    size_t partition_id;
    int leader_id; // 永远是 0
};

class MetadataCache {
private:
    mutable std::shared_mutex rw_lock_;

    // Topic -> (PartitionID -> Meta)
    std::unordered_map<std::string, std::unordered_map<size_t, PartitionMetadata>> topics_map_;

    // 单机模式下，我们只需要存自己的信息
    BrokerNode local_node_;

public:
    // 初始化时配置好自己的 IP 端口
    MetadataCache(std::string host, int port) {
        local_node_ = {LOCAL_BROKER_ID, host, port};
    }

    // --- 读操作 ---

    // 1. Client 问：这个 Topic 存在吗？有多少个分区？
    // 用于处理 MetadataRequest
    bool getTopicMetadata(const std::string& topic, std::vector<PartitionMetadata>& out_partitions) {
        std::shared_lock lock(rw_lock_);

        auto it = topics_map_.find(topic);
        if (it == topics_map_.end()) {
            return false; // Topic 不存在
        }

        for (const auto& kv : it->second) {
            out_partitions.push_back(kv.second);
        }
        return true;
    }

    // 2. Client 准备发送消息，问：Partition X 的 Leader 在哪？
    // 单机版逻辑：只要 Topic 和 Partition 存在，Leader 就是我！
    std::optional<BrokerNode> getPartitionLeader(const std::string& topic, int partition_id) {
        std::shared_lock lock(rw_lock_);

        auto topic_it = topics_map_.find(topic);
        if (topic_it == topics_map_.end()) return std::nullopt;

        auto& partitions = topic_it->second;
        if (partitions.find(partition_id) == partitions.end()) return std::nullopt;

        // 只要分区存在，直接返回我自己
        return local_node_;
    }

    bool get_partition_count(const std::string& topic,size_t& count){
        std::shared_lock lock(rw_lock_);
        auto topic_it = topics_map_.find(topic);
        if (topic_it == topics_map_.end()) return 0;

        count = topic_it->second.size();

        return 1;
    }

    // --- 写操作 (管理类操作) ---

    bool createTopic(const std::string& topic, size_t partition_count) {
            std::unique_lock lock(rw_lock_);

            // 1. 先检查是否存在
            if (topics_map_.find(topic) != topics_map_.end()) {
                return false; // 已存在，创建失败
            }

            auto& partitions = topics_map_[topic];

            // 3. 填充这个新 map
            for (size_t i = 0; i < partition_count; ++i) {
                partitions[i] = PartitionMetadata{i, LOCAL_BROKER_ID};
            }

            return true;
        }
};

#endif // CONTROLLER_H
