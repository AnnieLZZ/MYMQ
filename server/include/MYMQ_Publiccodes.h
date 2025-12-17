#ifndef MYMQ_PUBLICCODES_H
#define MYMQ_PUBLICCODES_H

#include <cstdint>
#include <string>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <functional>
#include <variant>
#include<memory>
namespace MYMQ_Public {

// ----------------------------------------------------------------------
// 1. 公共错误码定义 (对应 MYMQ::ErrorCode)
// ----------------------------------------------------------------------

// 注意：只包含您希望用户看到的错误码。
enum class CommonErrorCode : uint16_t {
    NULL_ERROR=4000,
    // MQ 错误
    GROUP_NOT_FOUND=4001,
    MEMBER_NOT_FOUND=4002,
    ILLEGAL_GENERATION=4003,
    REBALANCE_IN_PROGRESS=4004,
    AWAITING_LEADER_SYNC=4005,
    TOPIC_NOT_FOUND=4006,
    NO_RECORD=4007,
    EMPTY_GROUP=4008,
    NO_ASSIGNED_PARTITION=4009,
    USER_NOT_FOUND=4010,
    UNKNOWN_OFFSET_KEY=4011,
    TOPIC_EMPTY=4012,
    INTERNAL_ERROR=4013,
    CLIENT_LINK_NOT_FOUND=4014,
    CRC_VERIFY_FAILED=4015,
    FAILED_PARASE_PULL_DATA=4016,
    COMMIT_OFFSET_TIMEOUT=4017,
    REQUEST_TIMEOUT=4018,
    UNKNOWN_TOPICPARTITION=4019,
    CLIENT_NOT_IN_GROUP=4020,
    UPDATE_GENERATION=4021,
    GENERATION_EXPIRED=4022,



    // Logsegment 错误
    FULL_SEGMENT=5000,
    FAILED_ALLOCATE=5001,
    IO_ERROR=5002
};

// ServerErrorCode 的 to_string 函数
inline std::string to_string(CommonErrorCode code) {
    switch (code) {
    case CommonErrorCode::NULL_ERROR: return "NULL_ERROR";
    case CommonErrorCode::GROUP_NOT_FOUND: return "GROUP_NOT_FOUND";
    case CommonErrorCode::MEMBER_NOT_FOUND: return "MEMBER_NOT_FOUND";
    case CommonErrorCode::ILLEGAL_GENERATION: return "ILLEGAL_GENERATION";
    case CommonErrorCode::REBALANCE_IN_PROGRESS: return "REBALANCE_IN_PROGRESS";
    case CommonErrorCode::AWAITING_LEADER_SYNC: return "AWAITING_LEADER_SYNC";
    case CommonErrorCode::TOPIC_NOT_FOUND: return "TOPIC_NOT_FOUND";
    case CommonErrorCode::NO_RECORD: return "NO_RECORD";
    case CommonErrorCode::EMPTY_GROUP: return "EMPTY_GROUP";
    case CommonErrorCode::NO_ASSIGNED_PARTITION: return "NO_ASSIGNED_PARTITION";
    case CommonErrorCode::USER_NOT_FOUND: return "USER_NOT_FOUND";
    case CommonErrorCode::UNKNOWN_OFFSET_KEY: return "UNKNOWN_OFFSET_KEY";
    case CommonErrorCode::TOPIC_EMPTY: return "TOPIC_EMPTY";
    case CommonErrorCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
    case CommonErrorCode::CLIENT_LINK_NOT_FOUND: return "CLIENT_LINK_NOT_FOUND";
    case CommonErrorCode::CRC_VERIFY_FAILED: return "CRC_VERIFY_FAILED";
    case CommonErrorCode::FAILED_PARASE_PULL_DATA: return "FAILED_PARASE_PULL_DATA";
    case CommonErrorCode::COMMIT_OFFSET_TIMEOUT: return "COMMIT_OFFSET_TIMEOUT";
    case CommonErrorCode::REQUEST_TIMEOUT: return "REQUEST_TIMEOUT";
    case CommonErrorCode::UNKNOWN_TOPICPARTITION: return "UNKNOWN_TOPICPARTITION";
    case CommonErrorCode::CLIENT_NOT_IN_GROUP: return "CLIENT_NOT_IN_GROUP";
    case CommonErrorCode::FULL_SEGMENT: return "FULL_SEGMENT";
    case CommonErrorCode::FAILED_ALLOCATE: return "FAILED_ALLOCATE";
    case CommonErrorCode::IO_ERROR: return "IO_ERROR";
    default: return "UNKNOWN_SERVER_ERROR (" + std::to_string(static_cast<uint16_t>(code)) + ")";
    }
}
// ----------------------------------------------------------------------
// 2. 公共客户端错误码定义 (对应 MYMQ::MYMQ_Client::ErrorCode)
// ----------------------------------------------------------------------

enum class ClientErrorCode :uint16_t{
    NOT_IN_GROUP=1000,
    NULL_ERROR=1003,
    PULL_TIMEOUT=1004,
    PULL_OTHER_IN_PULL=1005,
    COMMIT_SYNC_TIMEOUT=1006,
    INVALID_TOPIC_PARTITION=1007,
    ZSTD_UNAVAILABLE=1008,
    AUTOCOMMIT_ENABLE=1009,
    INVALID_GROUPID=1010,
    UNKNOWN_ERROR=1011,
    EMPTY_RECORD=1012,
    INVALID_OPRATION=1013,
    REACHED_MAX_FLYING_REQUEST=1014,
    CRC_VERIFY_FAILED=1015,
    PARTIAL_PARASE_FAILED=1016,
    NOT_REGISTER=1017


};

// ClientErrorCode 的 to_string 函数
inline std::string to_string(ClientErrorCode code) {
    switch (code) {
    case ClientErrorCode::NOT_IN_GROUP: return "NOT_IN_GROUP";
    case ClientErrorCode::NULL_ERROR: return "NULL_ERROR";
    case ClientErrorCode::PULL_TIMEOUT: return "PULL_TIMEOUT";
    case ClientErrorCode::PULL_OTHER_IN_PULL: return "PULL_OTHER_IN_PULL";
    case ClientErrorCode::COMMIT_SYNC_TIMEOUT: return "COMMIT_SYNC_TIMEOUT";
    case ClientErrorCode::INVALID_TOPIC_PARTITION: return "INVALID_PARTITION_OR_TOPIC";
    case ClientErrorCode::ZSTD_UNAVAILABLE: return "ZSTD_UNAVAILABLE";
    case ClientErrorCode::AUTOCOMMIT_ENABLE: return "AUTOCOMMIT_ENABLE";
    case ClientErrorCode::INVALID_GROUPID: return "INVALID_GROUPID";
    case ClientErrorCode::UNKNOWN_ERROR: return "UNKNOWN_ERROR"; // <-- 补齐
    case ClientErrorCode::EMPTY_RECORD: return "EMPTY_RECORD"; // <-- 补齐
    case ClientErrorCode::INVALID_OPRATION: return "INVALID_OPRATION"; // <-- 补齐
    case ClientErrorCode::REACHED_MAX_FLYING_REQUEST: return "REACHED_MAX_FLYING_REQUEST"; // <-- 补齐
    case ClientErrorCode::CRC_VERIFY_FAILED: return "CRC_VERIFY_FAILED"; // <-- 补齐
    case ClientErrorCode::PARTIAL_PARASE_FAILED: return "PARTIAL_PARASE_FAILED"; // <-- 补齐
    case ClientErrorCode::NOT_REGISTER: return "NOT_REGISTER"; // <-- 补齐
    default: return "UNKNOWN_CLIENT_ERROR_CODE (" + std::to_string(static_cast<uint16_t>(code)) + ")";
    }
}


struct TopicPartition
{
    std::string topic;
    size_t partition;
    TopicPartition()=default;
    TopicPartition(const std::string& topic_,size_t partition_):topic(topic_),partition(partition_) {}
    bool operator==(const TopicPartition& other) const {
        return topic == other.topic && partition == other.partition;
    }
    bool operator<(const TopicPartition& other) const {
            // 先比较 topic
            if (topic != other.topic) {
                return topic < other.topic;
            }
            // 如果 topic 相同，则比较 partition
            return partition < other.partition;
        }
};


struct PushResponce {
    TopicPartition tp;
    CommonErrorCode errorcode;
    size_t offset;
    PushResponce(std::string topic,size_t partition,CommonErrorCode err,size_t offset_):tp(topic,partition),errorcode(err),offset(offset_){}
    PushResponce(const PushResponce& resp):tp(resp.tp),errorcode(resp.errorcode),offset(resp.offset) {}
};
struct CommitAsyncResponce {
    std::string groupid;
    TopicPartition tp;
    size_t committed_offset;
    CommonErrorCode error;
    CommitAsyncResponce(const CommitAsyncResponce& resp):groupid(resp.groupid),tp(resp.tp),committed_offset(resp.committed_offset),error(resp.error) {}
};


using PushResponceCallback = std::function<void(PushResponce)>;
using CommitAsyncResponceCallback = std::function<void(CommitAsyncResponce)>;

struct CallbackNoop {
    void operator()(CommonErrorCode) const {
        // 什么都不做，或者打印日志
    }

};

// 3. SupportedCallbacks 变体
using SupportedCallbacks = std::variant<
    PushResponceCallback,
    CommitAsyncResponceCallback,
    CallbackNoop
    >;


using ResultVariant = std::variant<
    PushResponce,
    CommitAsyncResponce,
    CommonErrorCode
    >;


template <typename T>
inline constexpr bool always_false_v = false;




class ConsumerRecord{
public:
    ConsumerRecord(const std::string& topic, size_t partition,
                   std::string_view key_view, std::string_view value_view,
                   int64_t time_, size_t offset,
                   std::shared_ptr<void> data_owner) // <--- 关键参数
        : tp(topic, partition), key(key_view), value(value_view),
        offset(offset), time(time_),
        owner(std::move(data_owner)) // 移动进来，增加引用计数
    {
    }
    ConsumerRecord()=default;

    std::string getTopic() const {
        return tp.topic;
    }
    size_t getPartition() const {
        return tp.partition;
    }
    size_t getOffset() const {
        return offset;
    }
    std::string getKey() const {
        return std::string(key) ;
    }
    std::string getValue() const {
        return std::string(value);
    }
    std::string_view getKey_view() const {
        return key ;
    }
    std::string_view getValue_view() const {
        return value;
    }
    int64_t getTime() const{
        return time;
    }

private:

private:
    TopicPartition tp;
    size_t offset;
    int64_t time;
    std::string_view key;
    std::string_view value;
    std::shared_ptr<void> owner;
};

} // namespace MYMQ_Public

namespace std {
template <>
struct hash<MYMQ_Public::TopicPartition> {

    std::size_t operator()(const MYMQ_Public::TopicPartition& tp) const noexcept {

        const std::size_t h1 = std::hash<std::string>{}(tp.topic);


        const std::size_t h2 = std::hash<size_t>{}(tp.partition);

       return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};
} // namespace std

struct TbbHashCompare {
    // 1. TBB 要求静态 hash 函数
    static size_t hash(const MYMQ_Public::TopicPartition& x) {
        // 这里直接复用你已经写好的 std::hash 特化
        return std::hash<MYMQ_Public::TopicPartition>{}(x);
    }

    // 2. TBB 要求静态 equal 函数
    static bool equal(const MYMQ_Public::TopicPartition& x, const MYMQ_Public::TopicPartition& y) {
        // 复用你已经写好的 operator==
        return x == y;
    }
};
#endif // MYMQ_PUBLICCODES_H
