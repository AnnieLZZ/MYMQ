#ifndef MYMQ_INNERCODES_H
#define MYMQ_INNERCODES_H

#include <cstdint> // For uint16_t
#include<string>
#include<set>
#include<map>
#include<vector>
#include<atomic>
#include<mutex>
#include<shared_mutex>
#include<condition_variable>
#include<chrono>
#include<unordered_map>
#include<cstring>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include<memory>
#include <errno.h>
#include<iostream>
#include <cstdint>
#include <numeric>
#include <vector>
#include<deque>
#include <string>
#include"zlib.h"
#include"zstd.h"
#include"../src/Serialize.h"
#include <ctime>
#include <iomanip>
#include<functional>
#include"MYMQ_Publiccodes.h"



namespace MYMQ { // 推荐使用命名空间进一步封装

const std::string consumeroffset_name="__consumer_offset";
const std::string clientid_DEFAULT="Client-1";
const std::string run_directory_DEFAULT="run";
constexpr uint16_t send_queue_size_DEFAULT=2048;
constexpr uint16_t HEADER_SIZE=12;
constexpr uint16_t HEARTBEAT_MS_CLIENT=2000;
constexpr uint16_t rebalance_timeout_ms=10000;
constexpr uint16_t join_collect_timeout_ms=5000;
constexpr uint16_t memberid_ready_timeout_s=3;
constexpr uint16_t commit_ready_timeout_s=3;
constexpr size_t pull_bytes_max=1048576;
constexpr uint16_t pollqueue_size_DEFAULT=2048;
constexpr uint16_t zstd_level_DEFAULT=9;
constexpr uint16_t ack_level_DEFAULT=1;
constexpr uint16_t MMAP_HEADER_SIZE = sizeof(size_t);
constexpr size_t MAX_ALLOWED_FILE_SIZE =2ULL * 1024 * 1024 * 1024;
constexpr size_t LOG_FLUSH_BYTES_INTERVAL_DEFAULT=65536;
constexpr size_t index_build_interval_bytes_DEFAULT=4096;
constexpr size_t LOG_FLUSH_INTERVAL_MS=360000;
constexpr size_t LOG_CLEAN_S_DEFAULT=144000;
constexpr uint16_t session_timeout_ms_=10000;

enum class EventType : uint16_t {
    // 客户端请求事件
    CLIENT_REQUEST_PUSH = 1001,      // 客户端发送消息到队列 [topicname(string)][partition(int)][msgbody(string)]
    CLIENT_REQUEST_PULL = 1002,      // 客户端从队列拉取消息 [groupid(string)][topicname(string)][partition(int)][offset(size_t)][msgbody(string)]
    CLIENT_REQUEST_COMMIT_OFFSET = 1003, // 客户端提交消费偏移量 [groupid(string)][topicname(string)][partition(int)][offset(size_t)][is_sync(bool)]
    CLIENT_REQUEST_JOIN_GROUP = 1004,    // 客户端加入消费者组 [groupid(string)][logicname(string)][memberid(string)][generationid(string)][host/IP(string)][topicnum(int)[topic1][topic2]...]
    CLIENT_REQUEST_LEAVE_GROUP = 1005,   // 客户端离开消费者组 [groupid(string)][memberid(string)]
    CLIENT_REQUEST_SYNC_GROUP = 1006,    // 客户端同步消费者组信息
    CLIENT_REQUEST_HEARTBEAT = 1007,     // 客户端发送心跳 [groupid(string)][memberid(string)]
    CLIENT_REQUEST_GET_TOPIC_PARTITIONS = 1008, // 客户端获取主题分区数量 [topicname(string)]
    CLIENT_REQUEST_CREATE_TOPIC=1009,
    CLIENT_REQUEST_REGISTER=1010,


    // 服务器响应事件 (可以与请求事件对应，或者有独立的响应码)
    SERVER_RESPONSE_SUCCESS = 2000,  // 通用成功响应
    SERVER_RESPONSE_ERROR = 2001,    // 通用错误响应
    SERVER_RESPONSE_PUSH_ACK = 2002, // 消息推送确认
    SERVER_RESPONSE_PULL_DATA = 2003, // 拉取消息数据
    SERVER_RESPONSE_JOIN_REQUEST_HANDLED = 2004, // 加入组响应
    SERVER_RESPONSE_SYNC_GROUP_ACK = 2005,
    SERVER_RESPONSE_GET_PARTITIONNUM =2006,
    SERVER_RESPONSE_REGISTER=2007,
    SERVER_RESPONSE_CREATE_TOPIC=2008,
    SERVER_RESPONCE_HEARTBEAT=2009,
    SERVER_RESPONCE_COMMIT_OFFSET=2010,
    SERVER_RESPONCE_LEAVE_GROUP=2011,

};

// 新增：EventType 的 to_string 函数
inline std::string to_string(EventType type) {
    switch (type) {
    // 客户端请求事件
    case EventType::CLIENT_REQUEST_PUSH: return "CLIENT_REQUEST_PUSH";
    case EventType::CLIENT_REQUEST_PULL: return "CLIENT_REQUEST_PULL";
    case EventType::CLIENT_REQUEST_COMMIT_OFFSET: return "CLIENT_REQUEST_COMMIT_OFFSET";
    case EventType::CLIENT_REQUEST_JOIN_GROUP: return "CLIENT_REQUEST_JOIN_GROUP";
    case EventType::CLIENT_REQUEST_LEAVE_GROUP: return "CLIENT_REQUEST_LEAVE_GROUP";
    case EventType::CLIENT_REQUEST_SYNC_GROUP: return "CLIENT_REQUEST_SYNC_GROUP";
    case EventType::CLIENT_REQUEST_HEARTBEAT: return "CLIENT_REQUEST_HEARTBEAT";
    case EventType::CLIENT_REQUEST_GET_TOPIC_PARTITIONS: return "CLIENT_REQUEST_GET_TOPIC_PARTITIONS";
    case EventType::CLIENT_REQUEST_CREATE_TOPIC: return "CLIENT_REQUEST_CREATE_TOPIC";
    case EventType::CLIENT_REQUEST_REGISTER: return "CLIENT_REQUEST_REGISTER";

        // 服务器响应事件
    case EventType::SERVER_RESPONSE_SUCCESS: return "SERVER_RESPONSE_SUCCESS";
    case EventType::SERVER_RESPONSE_ERROR: return "SERVER_RESPONSE_ERROR";
    case EventType::SERVER_RESPONSE_PUSH_ACK: return "SERVER_RESPONSE_PUSH_ACK";
    case EventType::SERVER_RESPONSE_PULL_DATA: return "SERVER_RESPONSE_PULL_DATA";
    case EventType::SERVER_RESPONSE_JOIN_REQUEST_HANDLED: return "SERVER_RESPONSE_JOIN_REQUEST_HANDLED";
    case EventType::SERVER_RESPONSE_SYNC_GROUP_ACK: return "SERVER_RESPONSE_SYNC_GROUP_ACK";
    case EventType::SERVER_RESPONSE_GET_PARTITIONNUM: return "SERVER_RESPONSE_GET_PARTITIONNUM";
    case EventType::SERVER_RESPONSE_REGISTER: return "SERVER_RESPONSE_REGISTER";
    case EventType::SERVER_RESPONSE_CREATE_TOPIC: return "SERVER_RESPONSE_CREATE_TOPIC";
    case EventType::SERVER_RESPONCE_HEARTBEAT: return "SERVER_RESPONCE_HEARTBEAT";
    case EventType::SERVER_RESPONCE_COMMIT_OFFSET: return "SERVER_RESPONCE_COMMIT_OFFSET";


    default: return "UNKNOWN_EVENT_TYPE (" + std::to_string(static_cast<uint16_t>(type)) + ")";
    }
}



enum class PullSet:uint16_t{
    END_OFFSET=0,
    EARLIEST_OFFSET=1
};

// 新增：PullSet 的 to_string 函数
inline std::string to_string(PullSet pullSet) {
    switch (pullSet) {
    case PullSet::END_OFFSET: return "END_OFFSET";
    case PullSet::EARLIEST_OFFSET: return "LATEST_OFFSET";
    default: return "UNKNOWN_PULL_SET (" + std::to_string(static_cast<uint16_t>(pullSet)) + ")";
    }
}


enum class ACK_Level:uint16_t{
    ACK_NORESPONCE=0,
    ACK_PROMISE_ACCEPT=1,
    ACK_PROMISE_INDISK=2
};

// 新增：ACK_Level 的 to_string 函数
inline std::string to_string(ACK_Level ackLevel) {
    switch (ackLevel) {
    case ACK_Level::ACK_NORESPONCE: return "ACK_NORESPONCE";
    case ACK_Level::ACK_PROMISE_ACCEPT: return "ACK_PROMISE_ACCEPT";
    case ACK_Level::ACK_PROMISE_INDISK: return "ACK_PROMISE_INDISK";
    default: return "UNKNOWN_ACK_LEVEL (" + std::to_string(static_cast<uint16_t>(ackLevel)) + ")";
    }
}

namespace ZSTD {


inline void check_zstd_error(size_t const zstd_result) {
    if (ZSTD_isError(zstd_result)) {
        throw std::runtime_error(std::string("ZSTD error: ") + ZSTD_getErrorName(zstd_result));
    }
}

// 压缩函数 (使用上下文)
inline std::vector<unsigned char> zstd_compress(
    ZSTD_CCtx* cctx, // 传入预创建的上下文
    const std::vector<unsigned char>& input_data,
    int compression_level
    ) {
    // 设置压缩级别 (每次压缩前可以更改)
    check_zstd_error(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, compression_level));

    // 计算输出缓冲区所需的最大大小
    size_t const compressed_buffer_size = ZSTD_compressBound(input_data.size());
    std::vector<unsigned char> compressed_data(compressed_buffer_size);

    // 执行压缩
    size_t const actual_compressed_size = ZSTD_compress2(
        cctx,
        compressed_data.data(), compressed_buffer_size,
        input_data.data(), input_data.size()
        );
    check_zstd_error(actual_compressed_size);

    compressed_data.resize(actual_compressed_size);
    return compressed_data;
}

// 解压缩函数 (使用上下文)
inline std::vector<unsigned char> zstd_decompress(
    ZSTD_DCtx* dctx, // 传入预创建的上下文
    const std::vector<unsigned char>& compressed_data
    ) {
    unsigned long long const decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("ZSTD_getFrameContentSize returned an error.");
    }
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("Original size unknown, cannot decompress with simple API.");
    }

    std::vector<unsigned char> decompressed_data(decompressed_size);

    size_t const actual_decompressed_size = ZSTD_decompressDCtx(
        dctx,
        decompressed_data.data(), decompressed_size,
        compressed_data.data(), compressed_data.size()
        );
    check_zstd_error(actual_decompressed_size);

    decompressed_data.resize(actual_decompressed_size);
    return decompressed_data;
}




}

namespace Crc32
{
static uint32_t calculate_crc32_impl(const unsigned char* data_ptr, size_t data_len)
{
    return static_cast<uint32_t>(crc32(0L, (const Bytef*)data_ptr, static_cast<uInt>(data_len)));
}

inline uint32_t calculate_crc32(const std::string& data)
{
    return calculate_crc32_impl(reinterpret_cast<const unsigned char*>(data.data()), data.length());
}

inline uint32_t calculate_crc32(const std::vector<unsigned char>& data)
{
    return calculate_crc32_impl(data.data(), data.size());
}

inline bool verify_crc32(const std::string& data, uint32_t expected_crc)//return 1表示没损坏
{
    uint32_t calculated_crc = calculate_crc32(data);
    return calculated_crc == expected_crc;
}

inline bool verify_crc32(const std::vector<unsigned char>& data, uint32_t expected_crc)
{
    uint32_t calculated_crc = calculate_crc32(data);
    return calculated_crc == expected_crc;
}

}

namespace MSG_serial {
struct Record{
    std::string key;
    std::string value;
    int64_t time;
    Record():key(""),value(""),time(0LL){}
    Record(std::string key_,std::string value_,int64_t time_):key(key_),value(value_),time(time_){}
};


inline std::vector<unsigned char> build_Record(const std::string& key,const std::string& value) {
    auto now = std::chrono::system_clock::now();
    int64_t current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    MessageBuilder mb;
    mb.reserve(2*sizeof(uint32_t)+key.size()+value.size()+sizeof(int64_t));
    mb.append(key,value,current_time_ms);
    uint32_t crc= MYMQ::Crc32::calculate_crc32(mb.data);
    MessageBuilder mb2;
    mb2.append(crc,mb.data);

    return mb2.data;
}

inline std::vector<unsigned char> build_Record(const Record& unpacked_msg) {
    MessageBuilder mb;
    mb.reserve(2*sizeof(uint32_t)+unpacked_msg.key.size()+unpacked_msg.value.size()+sizeof(int64_t));
    mb.append(unpacked_msg.key,unpacked_msg.value,unpacked_msg.time);
    uint32_t crc= MYMQ::Crc32::calculate_crc32(mb.data);
    MessageBuilder mb2;
    mb2.append(crc,mb.data);

    return mb2.data;
}

inline Record parase_Record(const std::vector<unsigned char>& binary_data) {
    if (binary_data.empty()) {
        return Record();
    }

    MessageParser mp(binary_data);
    auto crc=mp.read_uint32();
    auto msg=mp.read_uchar_vector();
    if(!MYMQ::Crc32::verify_crc32(msg,crc)){
        throw std::out_of_range("Crc verify failed");
    }
    try {
        MessageParser mp2(msg);
        auto key=mp2.read_string();
        auto value=mp2.read_string();
        auto time=mp2.read_ll();
        return Record(key, value, time);

    } catch (std::exception& e) {

       throw std::out_of_range("UNKNOWN ERROR IN RECORD PARASE");
    }



}


}

using ResponseCallback = std::function<void(uint16_t event_type, const std::vector<unsigned char>& msg_body)>;
struct PendingMessage {
    std::vector<unsigned char> message_bytes;
    size_t offset;
    uint32_t coid;
    ResponseCallback handler;
    PendingMessage()=default;
    PendingMessage(std::vector<unsigned char> bytes, uint32_t id, ResponseCallback cb)
        : message_bytes(std::move(bytes)), offset(0), coid(id), handler(std::move(cb)) {}
};


struct RecordBatch
{
    size_t baseoffset;
    size_t record_num;
    std::vector<unsigned char> records_body;
    RecordBatch()=default;
    RecordBatch(size_t baseoffset_,size_t record_num_,std::vector<unsigned char>& records_body_):
        baseoffset(baseoffset_),record_num(record_num_),records_body(records_body_){}
};

struct UserInfo
{
    std::string clientid;
    int sock;
    UserInfo():clientid(MYMQ::clientid_DEFAULT),sock(-1) {}
    UserInfo(const std::string& clientid_,int sock_):sock(sock_),clientid(clientid_){}
};


struct ConsumerInfo{
    std::set<std::string> subscribed_topics;
    std::string memberid;
    int generation_id;
    UserInfo userinfo;
    uint32_t correlation_id_lastjoin;
    ConsumerInfo(std::set<std::string> topics,std::string memberid,int generation_id,uint32_t correlation_id_lastjoin,std::string clientid=MYMQ::clientid_DEFAULT,int sock=-1)
        :subscribed_topics(topics),memberid(memberid),generation_id(generation_id),userinfo(clientid,sock),correlation_id_lastjoin(correlation_id_lastjoin){}
    ConsumerInfo():subscribed_topics(std::set<std::string>()),memberid(std::string()),generation_id(-1),userinfo(MYMQ::clientid_DEFAULT,-1),correlation_id_lastjoin(0){}
};

struct LeaderAssignmentData {
    std::string group_id;
    std::string leader_member_id;
    int generation_id;
    std::vector<ConsumerInfo> all_group_members; // 组内所有活跃成员及其订阅信息
    std::map<std::string, size_t> topic_partition_counts; // 主题名称 -> 分区总数
};


struct HeartbeatResponce{
    int generation_id;
    uint16_t groupstate_digit;//mapto {0,1,2,3} enum GroupState { STABLE, JOIN_COLLECTING, AWAITING_SYNC,EMPTY};
};



namespace MYMQ_Client{


struct ClientState {
    enum State {
        READING_ID,       // 正在读取客户端 ID (10 字节)
        READING_HEADER,   // 正在读取消息头 (HEADER_SIZE 字节)
        READING_BODY      // 正在读取消息体
    };

    using ClientMessageCallback = std::function<void(int client_fd, short event_type, const Byte& msg_body)>;
};

struct Consumerbasicinfo
{
    std::string groupid="";
    std::string clientid="";
    std::shared_mutex mtx;
    std::atomic<bool> is_ingroup=0;
};

using TopicPartition=MYMQ_Public::TopicPartition;
struct  Push_queue{
    std::mutex mtx;
    std::deque<std::vector<unsigned char>> queue_{};
    std::atomic<size_t>  since_last_send=0;
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    TopicPartition tp;
    Push_queue(const std::string& t,size_t p):tp(t,p){}
};

struct endoffset_point
{
    TopicPartition tp;
    size_t off;
    endoffset_point(size_t off_,const std::string& t,size_t p):tp(t,p),off(off_){}
};

}




}



#endif // MYMQ_INNERCODES_H
