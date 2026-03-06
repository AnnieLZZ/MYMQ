#include "MYMQ_C.h"
#include "MYMQ_Client.h"

using namespace MYMQ::Client;





MYMQ_Producer::MYMQ_Producer(const std::string& clientid, uint8_t ack_level)
    : pimpl(std::make_unique<MYMQ_Produceruse>(clientid, ack_level)) {
}


MYMQ_Producer::~MYMQ_Producer() = default;

Err_Client MYMQ_Producer::push(const MYMQ_Public::TopicPartition& tp,
                               const std::string& key,
                               const std::string& value,
                               MYMQ_Public::PushResponceCallback cb) {
    return pimpl->push(tp, key, value, cb);
}

void MYMQ_Producer::create_topic(std::string topicname, size_t parti_num) {
    pimpl->create_topic(topicname, parti_num);
}









MYMQ_Consumer::MYMQ_Consumer(const std::string& clientid, uint8_t ack_level)
    : pimpl(std::make_unique<MYMQ_Consumeruse>(clientid, ack_level))
{

}

MYMQ_Consumer::~MYMQ_Consumer() = default;

MYMQ_Consumer::MYMQ_Consumer(MYMQ_Consumer&&) noexcept = default;
MYMQ_Consumer& MYMQ_Consumer::operator=(MYMQ_Consumer&&) noexcept = default;


void  MYMQ_Consumer::set_pull_max_record_num_local(size_t num){

    return pimpl->set_pull_max_record_num_local(num);
}

void MYMQ_Consumer::set_pull_fetch_min_bytes(size_t bytes) {
    pimpl->set_pull_fetch_min_bytes(bytes);
}

MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::commit_async(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume
                                                       ,MYMQ_Public::CommitAsyncResponceCallback cb) {
    return pimpl->commit_async(tp,next_offset_to_consume,cb);
}

MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::seek(const MYMQ_Public::TopicPartition& tp,size_t offset_next_to_consume){
    return pimpl->seek(tp,offset_next_to_consume);
}


MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::pull(std::vector< MYMQ_Public::ConsumerRecord>& record_batch,size_t poll_wait_timeout_ms) {
    return pimpl->pull(record_batch,poll_wait_timeout_ms);
}

MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::pull(std::vector<MYMQ_Public::ConsumerRecord>& record_batch,
                     size_t poll_wait_timeout_ms,
                                  int64_t& out_latency_us){
    return pimpl->pull(record_batch,poll_wait_timeout_ms,out_latency_us);
}

void MYMQ_Consumer::trigger_pull(){
    return pimpl->trigger_poll_for_low_cap_pollbuffer();
}

void MYMQ_Consumer::create_topic(const std::string& topicname, size_t parti_num) {
    pimpl->create_topic(topicname, parti_num);
}
MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::get_position_consumed(const MYMQ_Public::TopicPartition& tp,size_t& pos){
    return pimpl->get_local_consumed_position(tp,pos);
}



void MYMQ_Consumer::subscribe_topic(const std::string& topicname) {
    pimpl->subscribe_topic(topicname);
}

void MYMQ_Consumer::unsubscribe_topic(const std::string& topicname) {
    pimpl->unsubscribe_topic(topicname);
}

MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::commit_sync(const MYMQ_Public::TopicPartition& tp, size_t next_offset_to_consume) {
    return pimpl->commit_sync(tp, next_offset_to_consume);
}


void MYMQ_Consumer::join_group(std::string groupid) {
    pimpl->join_group(groupid);
}

MYMQ_Consumer::ClientErrorCode MYMQ_Consumer::leave_group() {
   return pimpl->leave_group();
}



std::unordered_set<MYMQ_Public::TopicPartition> MYMQ_Consumer::get_assigned_partition() {
   return pimpl->get_assigned_partition();
}



bool MYMQ_Consumer::get_is_ingroup() {
    return pimpl->get_is_ingroup();
}
