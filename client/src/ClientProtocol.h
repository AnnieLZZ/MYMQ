#ifndef CLIENT_PROTOCOL_H
#define CLIENT_PROTOCOL_H

#include "MYMQ_PublicCodes.h"
#include "MYMQ_innercodes.h"
#include <vector>
#include <variant>
#include <string>

namespace MYMQ {
namespace Client {

struct PullResponseData {
    std::string topic;
    size_t partition;
    MYMQ_Public::CommonErrorCode error;
    size_t next_offset;
    std::vector<unsigned char> message_batch;
    size_t record_num;
};

struct LeaveGroupResponse {
    std::string group_id;
    MYMQ_Public::CommonErrorCode error;
};

struct PartitionAssignment {
    std::string topic;
    size_t partition;
    size_t end_offset;
};

struct HeartbeatResponse {
    MYMQ_Public::CommonErrorCode error;
    std::string group_id;
    std::string member_id;
    size_t generation_id;
    std::vector<PartitionAssignment> assignments;
};

struct CommitOffsetResponse {
    std::string group_id;
    std::string member_id;
    size_t generation_id;
    std::string topic;
    size_t partition;
    MYMQ_Public::CommonErrorCode error;
    size_t offset;
};

struct CreateTopicResponse {
    bool success;
};

using ProtocolResponse = std::variant<
    MYMQ_Public::PushResponce,
    MYMQ_Public::CommitAsyncResponce,
    MYMQ_Public::CommonErrorCode,
    PullResponseData,
    LeaveGroupResponse,
    HeartbeatResponse,
    CommitOffsetResponse,
    CreateTopicResponse
>;

class ClientProtocol {
public:
    static ProtocolResponse parse_response(MYMQ::EventType event_type, const std::vector<unsigned char>& msg_body);
    static MYMQ_Public::CommonErrorCode parse_record_batch(const std::vector<unsigned char>& raw_batch, ZSTD_DCtx* dctx, std::vector<MYMQ_Public::ConsumerRecord>& out_records, const MYMQ_Public::TopicPartition& tp);
    static std::vector<unsigned char> build_push_packet(
                    const std::string& topic,
                    uint64_t partition,
                    MYMQ::MSG_serial::BatchBuffer* src_buf,
                    ZSTD_CCtx* cctx,
                    int compression_level
                );

                static std::vector<unsigned char> build_create_topic_packet(const std::string& topic, size_t partition_num);
                static std::vector<unsigned char> build_leave_group_packet(const std::string& group_id, const std::string& member_id);
                static std::vector<unsigned char> build_heartbeat_packet(
                    const std::string& group_id,
                    const std::string& member_id,
                    size_t generation_id,
                    uint16_t pull_start_location,
                    bool is_join,
                    const std::string& client_id,
                    bool topics_updated,
                    const std::set<std::string>& topics
                );
                static std::vector<unsigned char> build_pull_packet(
                    const std::string& group_id,
                    const std::string& topic,
                    size_t partition,
                    size_t offset,
                    size_t max_bytes
                );
                static std::vector<unsigned char> build_commit_offset_packet(
                    const std::string& group_id,
                    const std::string& member_id,
                    size_t generation_id,
                    const std::string& topic,
                    size_t partition,
                    size_t offset
                );

                static std::vector<unsigned char> build_register_packet(const std::string& client_id);
};

}
}

#endif // CLIENT_PROTOCOL_H
