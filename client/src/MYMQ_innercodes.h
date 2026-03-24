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
#include <zstd.h>
#include"../src/Serialize.h"
#include <ctime>
#include <iomanip>
#include<functional>
#include"MYMQ_Publiccodes.h"
#include "BufferPool.h"
#include"tbb/concurrent_unordered_map.h"
#include<memory>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif


namespace MYMQ { // 推荐使用命名空间进一步封装

const std::string consumeroffset_name="__consumer_offset";
const std::string CLIENTID_DEFAULT="Client-1";
const std::string run_directory_DEFAULT=".";
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
constexpr size_t session_timeout_ms_=500000;
constexpr size_t MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT=1000;
constexpr size_t REQUEST_TIMEOUT_MS_DEFAULT=5000;

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
    EVENTTYPE_NULL=2012,

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
    ACK_PROMISE_INDISK=1,
};

// 新增：ACK_Level 的 to_string 函数
inline std::string to_string(ACK_Level ackLevel) {
    switch (ackLevel) {
    case ACK_Level::ACK_NORESPONCE: return "ACK_NORESPONCE";
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

inline std::vector<unsigned char> zstd_decompress_using_view(
    ZSTD_DCtx* dctx, // 传入预创建的上下文
    const unsigned char* data, size_t length
    ) {
    unsigned long long const decompressed_size = ZSTD_getFrameContentSize(data, length);

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
        data, length
        );
    check_zstd_error(actual_decompressed_size);

    decompressed_data.resize(actual_decompressed_size);
    return decompressed_data;
}


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

inline uint32_t calculate_crc32(const unsigned char* data, size_t length) {
    if (length == 0 || data == nullptr) {
        return UINT32_MAX;
    }
    return calculate_crc32_impl(data, length);
}

//
inline bool verify_crc32(const unsigned char* data, size_t length, uint32_t expected_crc) {
    uint32_t calculated_crc = calculate_crc32(data, length);
    return calculated_crc == expected_crc;
}

inline bool verify_crc32(const std::string& data, uint32_t expected_crc)//return 1表示没损坏
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








class BatchBuffer {//生产者用的
public:
    MYMQ::Client::BufferPool::Block block_{};
    unsigned char* data_ = nullptr;
    size_t capacity_ = 0;
    size_t write_pos_ = 0; // 当前写到了哪里
    size_t record_count_ = 0;
    int64_t first_timestamp_ = -1; // For calculating timestamp delta

    explicit BatchBuffer(size_t capacity) : capacity_(capacity) {
        write_pos_ = 0;
        first_timestamp_ = -1;
    }

    ~BatchBuffer() {
        release();
    }

    BatchBuffer(const BatchBuffer&) = delete;
    BatchBuffer& operator=(const BatchBuffer&) = delete;

    // 重置 Buffer（复用时调用，不释放内存）
    void clear() {
        write_pos_ = 0;
        record_count_ = 0;
        first_timestamp_ = -1;
    }

    bool ensure_allocated_for(std::chrono::milliseconds timeout) {
        if (data_ != nullptr && capacity_ > 0) return true;
        MYMQ::Client::BufferPool::Block blk;
        if (!MYMQ::Client::BufferPool::instance().try_allocate_for(capacity_, timeout, blk)) {
            return false;
        }
        block_ = blk;
        data_ = reinterpret_cast<unsigned char*>(block_.data);
        return true;
    }

    void release() {
        if (!block_.valid()) return;
        MYMQ::Client::BufferPool::instance().release(block_);
        data_ = nullptr;
        write_pos_ = 0;
        record_count_ = 0;
        first_timestamp_ = -1;
    }

    // 检查剩余空间是否足够
    bool has_capacity_for(size_t size_needed) const {
        return (write_pos_ + size_needed) <= capacity_;
    }

    // 返回有效数据大小
    size_t size() const { return write_pos_; }

    // 返回数据指针（给 ZSTD 用）
    const void* data_ptr() const { return data_; }

    // Helper: Calculate Varint Size (ZigZag)
    static size_t varint_size(int64_t value) {
        uint64_t n = (static_cast<uint64_t>(value) << 1) ^ (value >> 63);
        size_t len = 0;
        while (n >= 0x80) {
            len++;
            n >>= 7;
        }
        return len + 1;
    }

    // Helper: Write Varint (ZigZag) - Unsafe (Caller must check bounds)
    void write_varint_unsafe(int64_t value) {
        uint64_t n = (static_cast<uint64_t>(value) << 1) ^ (value >> 63);
        while (n >= 0x80) {
            data_[write_pos_++] = static_cast<unsigned char>((n & 0x7F) | 0x80);
            n >>= 7;
        }
        data_[write_pos_++] = static_cast<unsigned char>(n);
    }

    // --- 核心：替代 build_Record 的逻辑 ---
    // 返回 true 表示写入成功，false 表示空间不足
    bool append_record(const std::string& key, const std::string& value) {
        if (data_ == nullptr) return false;
        // 1. Calculate Timestamps
        auto now = std::chrono::system_clock::now();
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        if (first_timestamp_ == -1) {
            first_timestamp_ = ts;
        }
        int64_t ts_delta = ts - first_timestamp_;
        int64_t offset_delta = static_cast<int64_t>(record_count_);

        // 2. Calculate Sizes
        size_t key_len = key.size();
        size_t val_len = value.size();

        size_t sz_attr = 1;
        size_t sz_ts_delta = varint_size(ts_delta);
        size_t sz_off_delta = varint_size(offset_delta);
        size_t sz_key_len = varint_size(static_cast<int64_t>(key_len));
        size_t sz_val_len = varint_size(static_cast<int64_t>(val_len));
        size_t sz_headers = varint_size(0); // 0 headers

        // Body Size: Attributes + TS + Off + KeyLen + Key + ValLen + Val + Headers
        size_t body_size = sz_attr + sz_ts_delta + sz_off_delta + sz_key_len + key_len + sz_val_len + val_len + sz_headers;
        size_t sz_length = varint_size(static_cast<int64_t>(body_size));

        size_t total_size_needed = sz_length + body_size;

        // 3. Check Capacity
        if (write_pos_ + total_size_needed > capacity_) {
            return false;
        }

        // 4. Write Data
        // Length
        write_varint_unsafe(static_cast<int64_t>(body_size));
        
        // Attributes (0)
        data_[write_pos_++] = 0;

        // Timestamp Delta
        write_varint_unsafe(ts_delta);

        // Offset Delta
        write_varint_unsafe(offset_delta);

        // Key
        write_varint_unsafe(static_cast<int64_t>(key_len));
        if (key_len > 0) {
            std::memcpy(data_ + write_pos_, key.data(), key_len);
            write_pos_ += key_len;
        }

        // Value
        write_varint_unsafe(static_cast<int64_t>(val_len));
        if (val_len > 0) {
            std::memcpy(data_ + write_pos_, value.data(), val_len);
            write_pos_ += val_len;
        }

        // Headers (0)
        write_varint_unsafe(0);

        record_count_++;
        return true;
    }
 };



}

struct OwnedBytes {
    const unsigned char* data = nullptr;
    size_t size = 0;
    std::shared_ptr<void> owner{};
};

using ResponseCallback = std::function<void(uint16_t event_type, OwnedBytes msg_body)>;
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



struct HeartbeatResponce{
    int generation_id;
    uint16_t groupstate_digit;//mapto {0,1,2,3} enum GroupState { STABLE, JOIN_COLLECTING, AWAITING_SYNC,EMPTY};
};



namespace Client{


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
    std::set<std::string> subscribed_topics;
    std::string memberid="";
    size_t generation_id=0;
    std::shared_mutex mtx;
};

struct SparseCallback {
    uint32_t relative_index; // 该回调对应 Batch 中的第几条消息 (0-based)
    MYMQ_Public::SupportedCallbacks cb;
};

using TopicPartition=MYMQ_Public::TopicPartition;
using CallbackQueue = std::deque<MYMQ_Public::SupportedCallbacks>;
using BatchBuffer= MSG_serial::BatchBuffer;
struct BatchItem {
    std::unique_ptr<BatchBuffer> buffer;
    std::vector<SparseCallback> callbacks;
    size_t batch_count = 0;
};

struct Push_queue {
    std::mutex mtx;
    std::condition_variable cv_full;

    // Active buffer (currently being written to)
    std::unique_ptr<BatchBuffer> active_buf;
    std::vector<SparseCallback> active_cbs;
    size_t current_batch_count{0};

    // Ready queue (full buffers waiting to be flushed)
    std::deque<std::unique_ptr<BatchItem>> ready_queue;

    std::vector<std::unique_ptr<BatchBuffer>> free_pool;

    bool is_flushing = false; // Flag to ensure only one flush task per partition runs at a time

    ZSTD_CCtx* cctx = nullptr;
    TopicPartition tp;
    
    // Configuration
    size_t buffer_size_ = 1024 * 1024;
    size_t max_queued_batches_ = 5; // Allow 5 pending batches + 1 active

    Push_queue(const TopicPartition& tp, size_t buffer_size = 1024 * 1024, size_t max_queued_batches = 5)
        : tp(tp), buffer_size_(buffer_size), max_queued_batches_(max_queued_batches)
    {
        cctx = ZSTD_createCCtx();
        active_buf = std::make_unique<BatchBuffer>(buffer_size_);
    }

    ~Push_queue() {
        if (cctx) ZSTD_freeCCtx(cctx);
    }
};





struct Commitedoffset_point
{
    TopicPartition tp;
    mutable std::atomic<size_t> off;
    Commitedoffset_point(size_t off_,const TopicPartition& tp_):tp(tp_),off(off_){}
};


class PollBuffer {
    using Chuckitem= std::pair<size_t, OwnedBytes> ;
public:

    mutable std::atomic<size_t> local_consume_offset{0};
    enum State {
        PAUSE,
        NEED_POLL
    };

    PollBuffer(size_t low, size_t high)
        : low_level_capacity(low), high_level_capacity(high), state(NEED_POLL) {}


    bool need_poll() const {
        return state.load(std::memory_order_acquire) == NEED_POLL;
    }

    bool is_paused() const {
        return state.load(std::memory_order_acquire) == PAUSE;
    }
    void clear_for_seek(size_t target_offset){
        {
            std::lock_guard<std::mutex> ulock(mtx);
            {
                std::deque<Chuckitem> tmp{};
                std::swap(tmp,queue_);
            }

            curr_size.store(0);
            local_consume_offset.store(target_offset);
            state.store(NEED_POLL);
        }

    }

    bool try_pop(Chuckitem& target) {
        std::lock_guard<std::mutex> ulock(mtx);

        if (queue_.empty()) {
            return false;
        }

        auto& item = queue_.front();
        target = std::move(item);


        size_t popped_size = target.second.size;
        queue_.pop_front();

        size_t current = curr_size.fetch_sub(popped_size, std::memory_order_relaxed) - popped_size;

        // 迟滞判断
        // 只有当前是 PAUSE 且水位降到 LOW 以下，才“切换”状态
        if (state.load(std::memory_order_relaxed) == PAUSE && current <= low_level_capacity) {
            state.store(NEED_POLL, std::memory_order_release);
        }

        return true;
    }

    void push(OwnedBytes obj,size_t record_num_of_chuck) {
        std::lock_guard<std::mutex> ulock(mtx);

        size_t obj_size = obj.size;
        queue_.emplace_back(record_num_of_chuck,std::move(obj));

        // 更新大小
        size_t current = curr_size.fetch_add(obj_size, std::memory_order_relaxed) + obj_size;

        // 迟滞判断
        // 只有当前是 NEED_POLL 且水位涨到 HIGH 以上，才“切换”状态
        if (state.load(std::memory_order_relaxed) == NEED_POLL && current >= high_level_capacity) {
            state.store(PAUSE, std::memory_order_release);
        }
    }

private:
    std::mutex mtx;

    std::deque< Chuckitem  > queue_;

    // 使用 atomic 允许无锁查询
    std::atomic<size_t> curr_size{0};

    std::atomic<State> state;

    const size_t high_level_capacity;
    const size_t low_level_capacity;
};



class RecordAccumulator {
public:
    using PushQueuePtr = std::shared_ptr<Push_queue>;

    using PushqueueMap = tbb::concurrent_unordered_map<TopicPartition, std::shared_ptr<Push_queue>>;

private:
    PushqueueMap batches;

public:

    PushqueueMap::iterator begin() { return batches.begin(); }
    PushqueueMap::iterator end() { return batches.end(); }
    PushQueuePtr get_queue(const MYMQ_Public::TopicPartition& tp, size_t buffer_size, size_t max_queued_batches) {
        auto it = batches.find(tp);
        if (it != batches.end()) {
            return it->second;
        }
        auto new_queue = std::make_shared<Push_queue>(tp, buffer_size, max_queued_batches);

        // 原子插入
        auto result = batches.emplace(tp, new_queue);
        return result.first->second;
    }
};



struct TP_Point
{
    std::shared_ptr<Commitedoffset_point> endoffset_ptr=nullptr;
    std::shared_ptr<PollBuffer> pollqueue_ptr=nullptr;
};
}




}



#endif // MYMQ_INNERCODES_H
