#include "MYMQ_C.h"
#include "MYMQ_CLIENT.H"

MYMQ_Client::MYMQ_Client(const std::string& clientid, uint8_t ack_level)
    : pimpl(std::make_unique<MYMQ_clientuse>(clientid, ack_level))
{

}

MYMQ_Client::~MYMQ_Client() = default;

MYMQ_Client::MYMQ_Client(MYMQ_Client&&) noexcept = default;
MYMQ_Client& MYMQ_Client::operator=(MYMQ_Client&&) noexcept = default;




MYMQ_Client::ClientErrorCode MYMQ_Client::commit_async(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume
                                                       ,MYMQ_Public::SupportedCallbacks cb) {
    return pimpl->commit_async(tp,next_offset_to_consume);
}


MYMQ_Client::ClientErrorCode MYMQ_Client::push(const MYMQ_Public::TopicPartition& tp, const std::string& key, const std::string& value
                                               ,MYMQ_Public::SupportedCallbacks cb) {
    return pimpl->push(tp, key, value,cb);
}

MYMQ_Client::ClientErrorCode MYMQ_Client::pull(const MYMQ_Public::TopicPartition& tp,std::vector< MYMQ_Public::ConsumerRecord>& record_batch) {
    return pimpl->pull(tp,record_batch);
}

void MYMQ_Client::create_topic(const std::string& topicname, size_t parti_num) {
    pimpl->create_topic(topicname, parti_num);
}
size_t MYMQ_Client::get_position_consumed(const MYMQ_Public::TopicPartition& tp){
    return pimpl->get_position_consumed(tp);
}

void MYMQ_Client::set_pull_bytes(size_t bytes) {
    pimpl->set_pull_bytes(bytes);
}

void MYMQ_Client::subscribe_topic(const std::string& topicname) {
    pimpl->subscribe_topic(topicname);
}

void MYMQ_Client::unsubscribe_topic(const std::string& topicname) {
    pimpl->unsubscribe_topic(topicname);
}

MYMQ_Client::ClientErrorCode MYMQ_Client::commit_sync(const MYMQ_Public::TopicPartition& tp, size_t next_offset_to_consume) {
    return pimpl->commit_sync(tp, next_offset_to_consume);
}


void MYMQ_Client::join_group(const std::string& groupid) {
    pimpl->join_group(groupid);
}

MYMQ_Client::ClientErrorCode MYMQ_Client::leave_group(const std::string& groupid) {
   return pimpl->leave_group(groupid);
}



std::unordered_set<MYMQ_Public::TopicPartition> MYMQ_Client::get_assigned_partition() {
   return pimpl->get_assigned_partition();
}



bool MYMQ_Client::get_is_ingroup() {
    return pimpl->get_is_ingroup();
}
