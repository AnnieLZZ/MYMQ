#ifndef MYMQ_C_H
#define MYMQ_C_H

#include "MYMQ_PublicCodes.h" // Public types (ConsumerRecord, ErrorCodes)
#include <string>
#include <queue>
#include <utility> // For std::pair
#include <memory>  // For std::unique_ptr
#include <cstdint> // For uint8_t
#include<unordered_set>

class MYMQ_clientuse;

class MYMQ_Client {
public:

    using ConsumerRecord = MYMQ_Public::ConsumerRecord;
    using ClientErrorCode = MYMQ_Public::ClientErrorCode;
    using CommonErrorCode = MYMQ_Public::CommonErrorCode;



    MYMQ_Client(const std::string& clientid = std::string(), uint8_t ack_level = UINT8_MAX);

    ~MYMQ_Client();


    MYMQ_Client(const MYMQ_Client&) = delete;
    MYMQ_Client& operator=(const MYMQ_Client&) = delete;
    MYMQ_Client(MYMQ_Client&&) noexcept;
    MYMQ_Client& operator=(MYMQ_Client&&) noexcept;

    // --- Public API (Mirrored from MYMQ_clientuse) ---




    ClientErrorCode push(const MYMQ_Public::TopicPartition& tp, const std::string& key, const std::string& value
                         ,MYMQ_Public::SupportedCallbacks cb=MYMQ_Public::CallbackNoop{});
    ClientErrorCode pull(const MYMQ_Public::TopicPartition& tp,std::vector< MYMQ_Public::ConsumerRecord>& record_batch);

    ClientErrorCode seek(const MYMQ_Public::TopicPartition& tp,size_t offset_next_to_consume);
    ClientErrorCode commit_async(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::SupportedCallbacks cb=MYMQ_Public::CallbackNoop());
    void create_topic(const std::string& topicname, size_t parti_num = 1);
    void set_pull_bytes(size_t bytes);
    size_t get_position_consumed(const MYMQ_Public::TopicPartition& tp);

    void subscribe_topic(const std::string& topicname);
    void unsubscribe_topic(const std::string& topicname);

    ClientErrorCode commit_sync(const MYMQ_Public::TopicPartition& tp, size_t next_offset_to_consume);

    void join_group(const std::string& groupid);
    ClientErrorCode leave_group(const std::string& groupid);

    std::unordered_set<MYMQ_Public::TopicPartition> get_assigned_partition();

    bool get_is_ingroup();

private:
    std::unique_ptr<MYMQ_clientuse> pimpl;
};

#endif // MYMQ_C_H
