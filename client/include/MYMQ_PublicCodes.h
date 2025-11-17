#ifndef MYMQ_PUBLICCODES_H
#define MYMQ_PUBLICCODES_H

// --- 必需的 C++ 标准库头文件 ---
#include <cstdint>       // For uint16_t, size_t
#include <string>        // For std::string
#include <sstream>       // For ConsumerRecord::turn_time_to_string
#include <ctime>         // For std::time_t, std::tm, std::localtime
#include <iomanip>       // For std::put_time, std::setw, std::setfill
#include <functional>
#include <variant>
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



    // Logsegment 错误
    FULL_SEGMENT=5000,
    FAILED_ALLOCATE=5001,
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
    case CommonErrorCode::FULL_SEGMENT: return "FULL_SEGMENT";
    case CommonErrorCode::FAILED_ALLOCATE: return "FAILED_ALLOCATE";
    default: return "UNKNOWN_SERVER_ERROR (" + std::to_string(static_cast<uint16_t>(code)) + ")";
    }
}

// ----------------------------------------------------------------------
// 2. 公共客户端错误码定义 (对应 MYMQ::MYMQ_Client::ErrorCode)
// ----------------------------------------------------------------------

enum class ClientErrorCode :uint16_t{
    NOT_IN_GROUP=1000,
    INVALID_TOPIC=1001,
    INVALID_PARTITION=1002,
    NULL_ERROR=1003,
    PULL_TIMEOUT=1004,
    PULL_OTHER_IN_PULL=1005,
    COMMIT_SYNC_TIMEOUT=1006,
    INVALID_PARTITION_OR_TOPIC=1007,
    ZSTD_UNAVAILABLE=1008,
    AUTOCOMMIT_ENABLE=1009,
    INVALID_GROUPID=1010

};

// ClientErrorCode 的 to_string 函数
inline std::string to_string(ClientErrorCode code) {
    switch (code) {
    case ClientErrorCode::NOT_IN_GROUP: return "NOT_IN_GROUP";
    case ClientErrorCode::INVALID_TOPIC: return "INVALID_TOPIC";
    case ClientErrorCode::INVALID_PARTITION: return "INVALID_PARTITION";
    case ClientErrorCode::NULL_ERROR: return "NULL_ERROR";
    case ClientErrorCode::PULL_TIMEOUT: return "PULL_TIMEOUT";
    case ClientErrorCode::PULL_OTHER_IN_PULL: return "PULL_OTHER_IN_PULL";
    case ClientErrorCode::COMMIT_SYNC_TIMEOUT: return "COMMIT_SYNC_TIMEOUT";
    case ClientErrorCode::INVALID_PARTITION_OR_TOPIC: return "INVALID_PARTITION_OR_TOPIC";
    case ClientErrorCode::ZSTD_UNAVAILABLE: return "ZSTD_UNAVAILABLE";
    case ClientErrorCode::AUTOCOMMIT_ENABLE: return "AUTOCOMMIT_ENABLE";
    case ClientErrorCode::INVALID_GROUPID: return "INVALID_GROUPID";
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
};


struct PushResponce {
    TopicPartition tp;
    CommonErrorCode errorcode;
    PushResponce(std::string topic,size_t partition,CommonErrorCode err):tp(topic,partition),errorcode(err){}
    PushResponce(const PushResponce& resp):tp(resp.tp),errorcode(resp.errorcode) {}
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

using CallbackNoop = std::function<void(CommonErrorCode)>;

// 3. SupportedCallbacks 变体
using SupportedCallbacks = std::variant<
    PushResponceCallback,
    CommitAsyncResponceCallback,
    CallbackNoop
    >;
inline const CallbackNoop TheNoopCallback = [](CommonErrorCode ){
};

inline const SupportedCallbacks DefaultNoopVariant{TheNoopCallback};

using ResultVariant = std::variant<
    PushResponce,
    CommitAsyncResponce,
    CommonErrorCode
    >;


template <typename T>
inline constexpr bool always_false_v = false;




class ConsumerRecord{
public:
    ConsumerRecord(const std::string& topic ,size_t partition,const std::string& key,const std::string& value ,int64_t time_,size_t offset)
        :tp(topic,partition),key(key),value(value),offset(offset),time(time_){
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
        return key;
    }
    std::string getValue() const {
        return value;
    }
    int64_t getTime() const{
        return time;
    }

private:

    TopicPartition tp;
    size_t offset;
    std::string key;
    std::string value;
    int64_t time;
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

#endif // MYMQ_PUBLICCODES_H
