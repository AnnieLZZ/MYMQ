#include "MYMQ_S.h"

#include "MessageQueue.h"
MYMQ_S::MYMQ_S()
    : pimpl(std::make_unique<MessageQueue>("MYMQ_DEFAULT_DIR"))
{

}


// 析构函数：必须在此定义，以允许 std::unique_ptr 销毁不完整类型
MYMQ_S::~MYMQ_S() = default;

// --- 所有公共API的转发实现 ---

size_t MYMQ_S::get_partition_endoffset(const std::string& topicname, size_t partition)
{
    return pimpl->get_partition_endoffset(topicname, partition);
}

void MYMQ_S::load_topics_metadata()
{
    pimpl->load_topics_metadata();
}

void MYMQ_S::save_topics_metadata()
{
    pimpl->save_topics_metadata();
}

Err MYMQ_S::push(Mybyte& msg, const std::string& topicname, size_t partition)
{
    return pimpl->push(msg, topicname, partition);
}

std::pair<MesLoc, Err> MYMQ_S::pull(size_t target_offset, const std::string& groupid, const std::string& topicname, size_t partition, size_t byte_need)
{
    return pimpl->pull(target_offset, groupid, topicname, partition, byte_need);
}

bool MYMQ_S::create_topic(const std::string& topicname, size_t parti_num)
{
    return pimpl->create_topic(topicname, parti_num);
}

Err MYMQ_S::commit_sync(const std::string& topicname, size_t partition, uint32_t consumeroffset_parid_hash, const std::string& key, uint32_t offset_digit)
{
    return pimpl->commit_sync(topicname, partition, consumeroffset_parid_hash, key, offset_digit);
}

void MYMQ_S::clear_partition(const std::string& topicname, size_t partition)
{
    pimpl->clear_partition(topicname, partition);
}

std::pair<int, Err> MYMQ_S::joinGroup(const std::string& groupid, ConsumerInfo& inf)
{
    return pimpl->joinGroup(groupid, inf);
}

Err MYMQ_S::leave_group(const std::string& groupid, const std::string& memberid)
{
    return pimpl->leave_group(groupid, memberid);
}

std::pair<std::map<std::string, std::set<size_t>>, Err> MYMQ_S::sync_group(
    const std::string& group_id,
    const std::string& member_id,
    int generation_id,
    const std::map<std::string, std::map<std::string, std::set<size_t>>>& leader_assignments)
{
    return pimpl->sync_group(group_id, member_id, generation_id, leader_assignments);
}

HeartbeatResponce MYMQ_S::heartbeat(const std::string& group_id, const std::string& member_id)
{
    return pimpl->heartbeat(group_id, member_id);
}

std::pair<ConsumerGroupState::GroupState, bool> MYMQ_S::get_SpecificState(const std::string& groupid)
{
    return pimpl->get_SpecificState(groupid);
}

