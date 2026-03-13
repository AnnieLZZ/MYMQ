#include "ClientProtocol.h"
#include "MYMQ_innercodes.h"
#include "Serialize.h"
#include "MurmurHash2.h"
#include <iostream>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace MYMQ {
namespace Client {

// Helper for htonll if not defined
#ifndef htonll
#ifdef _WIN32
#define htonll(x) ((((uint64_t)htonl(x)) << 32) + htonl((x) >> 32))
#else
#define htonll(x) ((((uint64_t)htonl(x)) << 32) + htonl((x) >> 32))
#endif
#endif

ProtocolResponse ClientProtocol::parse_response(MYMQ::EventType event_type, const std::vector<unsigned char>& msg_body) {
    if (msg_body.empty() && event_type != MYMQ::EventType::CLIENT_REQUEST_HEARTBEAT) {
        return MYMQ_Public::CommonErrorCode::UNKNOWN_SERVER_ERROR;
    }

    try {
        MessageParser parser(msg_body.data(), msg_body.size());

        switch (event_type) {
            case MYMQ::EventType::SERVER_RESPONSE_PUSH_ACK: {
                 std::string topic = parser.read_string();
                 uint64_t partition = parser.read_uint64();
                 auto err = static_cast<MYMQ_Public::CommonErrorCode>(parser.read_uint16());
                 uint64_t offset = parser.read_uint64();
                 return MYMQ_Public::PushResponce(topic, partition, err, offset);
            }
            case MYMQ::EventType::SERVER_RESPONCE_COMMIT_OFFSET: {
                 std::string groupid = parser.read_string();
                 std::string topic = parser.read_string();
                 uint64_t partition = parser.read_uint64();
                 auto err = static_cast<MYMQ_Public::CommonErrorCode>(parser.read_uint16());
                 uint64_t offset = parser.read_uint64();
                 
                 MYMQ_Public::CommitAsyncResponce r{groupid, {topic, partition}, offset, err};
                 return r;
            }
            case MYMQ::EventType::SERVER_RESPONSE_PULL_DATA: {
                 std::string topic = parser.read_string();
                 uint64_t partition = parser.read_uint64();
                 auto err = static_cast<MYMQ_Public::CommonErrorCode>(parser.read_uint16());
                 uint64_t next_offset = parser.read_uint64();
                 uint64_t data_len = parser.read_uint64();

                 std::vector<unsigned char> batch;
                 if (data_len > 0) {
                     if (parser.get_remaining_bytes() < data_len) {
                         return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
                     }
                     const auto* ptr = parser.get_current_ptr();
                     batch.assign(ptr, ptr + data_len);
                 }

                 return PullResponseData{topic, partition, err, next_offset, batch, 0};
            }
            case MYMQ::EventType::SERVER_RESPONCE_LEAVE_GROUP: {
                 auto err = static_cast<MYMQ_Public::CommonErrorCode>(parser.read_uint16());
                 std::string group_id = parser.read_string();
                 return LeaveGroupResponse{group_id, err};
            }
            case MYMQ::EventType::SERVER_RESPONCE_HEARTBEAT: {
                     auto err = static_cast<MYMQ_Public::CommonErrorCode>(parser.read_uint16());
                     std::string group_id = parser.read_string();
                     std::string member_id = parser.read_string();
                     size_t generation_id = parser.read_size_t();
                     
                     std::vector<PartitionAssignment> assignments;
                     if (err == MYMQ_Public::CommonErrorCode::UPDATE_GENERATION) {
                         size_t topic_count = parser.read_size_t(); 
                         for(size_t i=0; i<topic_count; ++i) {
                             std::string topic = parser.read_string();
                             size_t partition_count = parser.read_size_t();
                             for(size_t j=0; j<partition_count; ++j){
                                 size_t part = parser.read_size_t();
                                 size_t end_off = parser.read_size_t();
                                 assignments.push_back({topic, part, end_off});
                             }
                         }
                     }
                     return HeartbeatResponse{err, group_id, member_id, generation_id, assignments};
                }
            case MYMQ::EventType::SERVER_RESPONSE_CREATE_TOPIC: {
                bool success = parser.read_bool();
                return CreateTopicResponse{success};
            }
            default:
                 return MYMQ_Public::CommonErrorCode::UNKNOWN_SERVER_ERROR;
        }
    } catch (...) {
        return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
    }
}

MYMQ_Public::CommonErrorCode ClientProtocol::parse_record_batch(const std::vector<unsigned char>& raw_batch, ZSTD_DCtx* dctx, std::vector<MYMQ_Public::ConsumerRecord>& out_records, const MYMQ_Public::TopicPartition& tp) {
    if (raw_batch.size() < 12) return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;

    MessageParser parser(raw_batch.data(), raw_batch.size());
    
    // Header Parsing
    // 1. Batch Base Offset (8)
    uint64_t base_offset = parser.read_uint64();

    // 2. Batch Length (4)
    uint32_t batch_length = parser.read_uint32();

    // 3. Partition Leader Epoch (4)
    uint32_t partition_leader_epoch = parser.read_uint32();

    // 4. Magic (1)
    uint8_t magic = parser.read_byte();

    // 5. CRC (4)
    uint32_t crc = parser.read_uint32();

    // 6. Attributes (2)
    uint16_t attributes = parser.read_uint16();
    // Bit 0~2: Compression Codec
    int8_t compression_codec = attributes & 0x07;

    // 7. Last Offset Delta (4)
    int32_t last_offset_delta = parser.read_int32();

    // 8. First Timestamp (8)
    int64_t first_timestamp = parser.read_int64();

    // 9. Max Timestamp (8)
    int64_t max_timestamp = parser.read_int64();

    // 10. Producer ID (8)
    int64_t producer_id = parser.read_int64();

    // 11. Producer Epoch (2)
    int16_t producer_epoch = parser.read_short();

    // 12. Base Sequence (4)
    int32_t base_sequence = parser.read_int32();

    // 13. Records Count (4)
    uint32_t records_count = parser.read_uint32();

    // ----------------------------------------------------------------------
    // Compression Handling
    // ----------------------------------------------------------------------
    const unsigned char* records_data_ptr = parser.get_current_ptr();
    size_t records_data_size = parser.get_remaining_bytes();

    std::vector<unsigned char> decompressed_buffer;
    bool is_compressed = (compression_codec != 0);

    if (is_compressed) {
        if (compression_codec == 1) { // ZSTD
             // ZSTD Decompression
            unsigned long long const rSize = ZSTD_getFrameContentSize(records_data_ptr, records_data_size);
             if (rSize == ZSTD_CONTENTSIZE_ERROR) {
                return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
            }
            if (rSize == ZSTD_CONTENTSIZE_UNKNOWN) {
                 return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
            }

            decompressed_buffer.resize(rSize);
            
            size_t const dSize = ZSTD_decompressDCtx(dctx, decompressed_buffer.data(), rSize, records_data_ptr, records_data_size);
            
            if (ZSTD_isError(dSize)) {
                return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
            }

            // Update parser to point to decompressed data
            parser = MessageParser(decompressed_buffer.data(), decompressed_buffer.size());

        } else {
            // Unknown compression
            return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
        }
    } 
    // If not compressed, parser continues from records_data_ptr (already set)

    // ----------------------------------------------------------------------
    // Records Iteration
    // ----------------------------------------------------------------------
    out_records.reserve(records_count);
    
    // Shared pointer to owner (original batch or decompressed buffer)
    // If compressed, owner is the decompressed_buffer (we need to keep it alive)
    // If not compressed, owner is likely the raw_batch passed in? 
    // Actually, `raw_batch` is const ref, we can't share ownership of it easily unless we copy it or the caller keeps it.
    // The `ConsumerRecord` expects `std::shared_ptr<void> data_owner`.
    // We should copy the data if we want to be safe, OR we rely on the fact that `out_records` 
    // will copy string_views into strings? 
    // Wait, `ConsumerRecord` stores `string_view` AND `std::shared_ptr<void> owner`.
    // So we MUST provide an owner that holds the data.
    
    std::shared_ptr<std::vector<unsigned char>> data_owner;
    if (is_compressed) {
        data_owner = std::make_shared<std::vector<unsigned char>>(std::move(decompressed_buffer));
        parser = MessageParser(data_owner->data(), data_owner->size());
    } else {
        // If not compressed, the data is in `raw_batch`. 
        // We need to copy it to a shared_ptr because `raw_batch` lifespan is not guaranteed beyond this function.
        // Or we copy the relevant part.
        // Ideally we should have a zero-copy mechanism from the network buffer.
        // For now, let's copy the raw batch to ensure safety.
        data_owner = std::make_shared<std::vector<unsigned char>>(raw_batch);
        // We need to advance parser to the records part in the NEW copy
        parser = MessageParser(data_owner->data(), data_owner->size());
        // Skip header
        parser.skip(8+4+4+1+4+2+4+8+8+8+2+4+4); 
    }

    const unsigned char* base_ptr = is_compressed ? data_owner->data() : data_owner->data(); // Correct base for offsets

    for (uint32_t i = 0; i < records_count; ++i) {
        try {
            // Record Parsing (Varint Lengths)
            size_t record_start_offset = parser.get_offset();
            
            // 1. Length (signed varint)
            int64_t length = parser.read_varint(); 
            
            // 2. Attributes (1 byte)
            int8_t attributes = parser.read_byte();
            
            // 3. Timestamp Delta (signed varint)
            int64_t timestamp_delta = parser.read_varint();
            
            // 4. Offset Delta (signed varint)
            int64_t offset_delta = parser.read_varint();
            
            // 5. Key Length (signed varint)
            int64_t key_length = parser.read_varint();
            const char* key_ptr = nullptr;
            if (key_length >= 0) {
                key_ptr = reinterpret_cast<const char*>(parser.get_current_ptr());
                parser.skip(key_length);
            }
            
            // 6. Value Length (signed varint)
            int64_t value_length = parser.read_varint();
            const char* value_ptr = nullptr;
            if (value_length >= 0) {
                value_ptr = reinterpret_cast<const char*>(parser.get_current_ptr());
                parser.skip(value_length);
            }
            
            // 7. Headers (Varint count)
            int64_t header_count = parser.read_varint();
            for (int h = 0; h < header_count; ++h) {
                int64_t header_key_len = parser.read_varint();
                parser.skip(header_key_len);
                int64_t header_val_len = parser.read_varint();
                parser.skip(header_val_len);
            }

            // Construct Record
            std::string_view key_view = (key_length > 0) ? std::string_view(key_ptr, key_length) : std::string_view();
            std::string_view value_view = (value_length > 0) ? std::string_view(value_ptr, value_length) : std::string_view();
            
            out_records.emplace_back(
                tp.topic, // Topic (filled by caller)
                tp.partition,  // Partition (filled by caller)
                key_view,
                value_view,
                first_timestamp + timestamp_delta,
                base_offset + offset_delta,
                data_owner
            );

        } catch (...) {
            return MYMQ_Public::CommonErrorCode::FAILED_PARASE_PULL_DATA;
        }
    }

    return MYMQ_Public::CommonErrorCode::Success;
}

std::vector<unsigned char> ClientProtocol::build_create_topic_packet(const std::string& topic, size_t partition_num) {
    MessageBuilder mb;
    mb.append_string(topic);
    mb.append_size_t(partition_num);
    return mb.data;
}

std::vector<unsigned char> ClientProtocol::build_leave_group_packet(const std::string& group_id, const std::string& member_id) {
    MessageBuilder mb;
    mb.append_string(group_id);
    mb.append_string(member_id);
    return mb.data;
}

std::vector<unsigned char> ClientProtocol::build_heartbeat_packet(
    const std::string& group_id,
    const std::string& member_id,
    size_t generation_id,
    uint16_t pull_start_location,
    bool is_join,
    const std::string& client_id,
    bool topics_updated,
    const std::set<std::string>& topics
) {
    MessageBuilder mb;
    mb.append_string(group_id);
    mb.append_string(member_id);
    mb.append_size_t(generation_id);
    mb.append_uint16(pull_start_location);
    
    if (is_join) {
        mb.append_string(client_id);
    }
    
    mb.append_bool(topics_updated);
    
    if (topics_updated) {
        mb.append_size_t(topics.size());
        for (const auto& t : topics) {
            mb.append_string(t);
        }
    }
    
    return mb.data;
}

std::vector<unsigned char> ClientProtocol::build_pull_packet(
    const std::string& group_id,
    const std::string& topic,
    size_t partition,
    size_t offset,
    size_t max_bytes
) {
    MessageBuilder mb;
    mb.append_string(group_id);
    mb.append_string(topic);
    mb.append_size_t(partition);
    mb.append_size_t(offset);
    mb.append_size_t(max_bytes);
    return mb.data;
}

std::vector<unsigned char> ClientProtocol::build_commit_offset_packet(
    const std::string& group_id,
    const std::string& member_id,
    size_t generation_id,
    const std::string& topic,
    size_t partition,
    size_t offset
) {
    // 1. Construct Key for server (Group + Topic + Partition)
    std::string key_gtp;
    {
        MessageBuilder mb_key;
        mb_key.append_string(group_id);
        mb_key.append_string(topic);
        mb_key.append_size_t(partition);
        key_gtp = mb_key.dump();
    }

    // 2. Calculate Group ID Hash
    // Note: Default seed 0 is used in MYMQ_Client.cpp's implicit usage
    uint32_t parid_hash = MurmurHash2::hash(group_id);

    // 3. Build Final Packet
    MessageBuilder mb;
    mb.append_string(group_id);
    mb.append_string(member_id);
    mb.append_size_t(generation_id);
    mb.append_string(topic);
    mb.append_size_t(partition);
    mb.append_uint32(parid_hash); // Assuming parid_hash is uint32_t from MurmurHash2
    mb.append_string(key_gtp);
    mb.append_size_t(offset);

    return mb.data;
}

std::vector<unsigned char> ClientProtocol::build_push_packet(
        const std::string& topic,
        uint64_t partition,
        MYMQ::MSG_serial::BatchBuffer* src_buf,
        ZSTD_CCtx* cctx,
        int compression_level
    ) {
    MessageBuilder mb_recordbatch;

    // Start of RecordBatch
    size_t batch_start_idx = mb_recordbatch.data.size();
    
    // --- Batch Header Placeholders ---
    // 1. Base Offset (8)
    mb_recordbatch.append_int64(0); 
    
    // 2. Batch Length (4) - Placeholder
    size_t batch_len_offset = mb_recordbatch.data.size();
    mb_recordbatch.append_int32(0);
    
    // 3. Partition Leader Epoch (4)
    mb_recordbatch.append_int32(0);
    
    // 4. Magic (1)
    mb_recordbatch.append_byte(2);
    
    // 5. CRC (4) - Placeholder
    size_t crc_offset = mb_recordbatch.data.size();
    mb_recordbatch.append_uint32(0);
    
    // 6. Attributes (2)
    // Bit 0~2: Compression Codec (0=None, 1=ZSTD)
    int16_t attributes = 0;
    if (compression_level > 0 && src_buf->size() > 0) {
        attributes |= 1; // ZSTD
    }
    mb_recordbatch.append_int16(attributes);
    
    // 7. Last Offset Delta (4)
    int32_t last_offset_delta = (src_buf->record_count_ > 0) ? (static_cast<int32_t>(src_buf->record_count_) - 1) : 0;
    mb_recordbatch.append_int32(last_offset_delta);
    
    // 8. First Timestamp (8)
    int64_t first_ts = src_buf->first_timestamp_;
    if (first_ts == -1) first_ts = 0; // Should handle empty batch case
    mb_recordbatch.append_int64(first_ts);
    
    // 9. Max Timestamp (8) - Approximated as FirstTimestamp for now (or could be current time)
    // ideally BatchBuffer should track max timestamp.
    mb_recordbatch.append_int64(first_ts);
    
    // 10. Producer ID (8)
    mb_recordbatch.append_int64(-1);
    
    // 11. Producer Epoch (2)
    mb_recordbatch.append_int16(-1);
    
    // 12. Base Sequence (4)
    mb_recordbatch.append_int32(-1);
    
    // 13. Records Count (4)
    mb_recordbatch.append_uint32(static_cast<uint32_t>(src_buf->record_count_));
    
    // --- Records Body ---
    if (compression_level > 0 && src_buf->size() > 0) {
        // Compress src_buf->data_
        size_t zstd_bound = ZSTD_compressBound(src_buf->size());
        size_t current_size = mb_recordbatch.data.size();
        mb_recordbatch.data.resize(current_size + zstd_bound);
        
        size_t compressed_size = ZSTD_compressCCtx(
            cctx, 
            mb_recordbatch.data.data() + current_size, 
            zstd_bound,
            src_buf->data_ptr(), 
            src_buf->size(),
            compression_level
        );
        
        if (ZSTD_isError(compressed_size)) {
            return {}; // Error
        }
        mb_recordbatch.data.resize(current_size + compressed_size);
    } else {
        // Raw Copy
        size_t current_size = mb_recordbatch.data.size();
        mb_recordbatch.data.resize(current_size + src_buf->size());
        std::memcpy(mb_recordbatch.data.data() + current_size, src_buf->data_ptr(), src_buf->size());
    }
    
    // --- Fill Placeholders ---
    
    // A. Batch Length: From PartitionLeaderEpoch to End
    // Batch Start is at batch_start_idx.
    // Length field is at batch_len_offset (size 4).
    // Length counts bytes AFTER the Length field.
    size_t total_batch_size = mb_recordbatch.data.size() - batch_start_idx;
    // Length field is 8 bytes into the batch (BaseOffset is 8).
    // So Length value = total_batch_size - 8 - 4 = total_batch_size - 12.
    int32_t batch_length_val = static_cast<int32_t>(total_batch_size - 12);
    
    // Rewrite Batch Length (Big Endian)
    uint32_t n_batch_len = htonl(batch_length_val);
    std::memcpy(mb_recordbatch.data.data() + batch_len_offset, &n_batch_len, sizeof(uint32_t));
    
    // B. CRC: Covers Attributes (offset 21 from batch start) to End
    // Header structure:
    // BaseOffset(8) + Length(4) + Epoch(4) + Magic(1) + CRC(4) + Attributes(2)...
    // 8+4+4+1+4 = 21 bytes. Attributes starts at index 21 relative to batch start.
    size_t crc_start_idx = batch_start_idx + 21; 
    size_t crc_len = mb_recordbatch.data.size() - crc_start_idx;
    
    uint32_t crc_val = MYMQ::Crc32::calculate_crc32(mb_recordbatch.data.data() + crc_start_idx, crc_len);
    // CRC32C usually, but let's stick to what Crc32 namespace provides.
    
    // Rewrite CRC (Big Endian)
    uint32_t n_crc = htonl(crc_val);
    std::memcpy(mb_recordbatch.data.data() + crc_offset, &n_crc, sizeof(uint32_t));

    const uint32_t outer_crc = MYMQ::Crc32::calculate_crc32(mb_recordbatch.data.data(), mb_recordbatch.data.size());

    MessageBuilder mb;
    mb.append_string(topic);
    mb.append_uint64(partition);
    mb.append_uint32(outer_crc);
    mb.append_uchar_vector(mb_recordbatch.data);
    return mb.data;
}

std::vector<unsigned char> ClientProtocol::build_register_packet(const std::string& client_id) {
    MessageBuilder mb;
    mb.append_string(client_id);
    return mb.data;
}

}
}
