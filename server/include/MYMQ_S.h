#ifndef MYMQ_S_H
#define MYMQ_S_H
#pragma once

#include <memory> // 为了 std::unique_ptr
#include "MYMQ_Publiccodes.h" // 包含公共定义
#include"MYMQ_innercodes.h"
#include"MYMQ_Server_ns.h"

using Record=MYMQ::MSG_serial::Record;
using HeartbeatResponce=MYMQ::HeartbeatResponce;
using Err=MYMQ_Public::CommonErrorCode;
using MB= MessageBuilder;
using MP=MessageParser;
using MesLoc=MYMQ_Server::MessageLocation;
using Mybyte=std::vector<unsigned char>;
using Eve=MYMQ::EventType;
class MessageQueue;

class MYMQ_S
{
public:
    // 构造函数和析构函数
    MYMQ_S();
    ~MYMQ_S(); // 必须在 .cpp 中定义，以便 unique_ptr 知道如何销毁 Impl

    MYMQ_S(const MYMQ_S&) = delete;
    MYMQ_S& operator=(const MYMQ_S&) = delete;


    MYMQ_S(MYMQ_S&&) = delete;
    MYMQ_S& operator=(MYMQ_S&&) = delete;


    size_t get_partition_endoffset(const std::string& topicname,size_t partition );
    void load_topics_metadata() ;



    void save_topics_metadata() ;
    Err push(Mybyte& msg ,const std::string& topicname,size_t partition) ;
    std::pair<MesLoc,Err>  pull(size_t target_offset,const std::string& groupid,const std::string& topicname, size_t partition,size_t byte_need) ;
    bool create_topic(const std::string& topicname,size_t parti_num =1);

    Err commit_sync(const std::string& topicname, size_t partition ,uint32_t consumeroffset_parid_hash,const std::string& key,uint32_t offset_digit) ;



    void clear_partition(const std::string& topicname,size_t partition) ;

    Err leave_group(const std::string& groupid,const std::string& memberid);

    std::pair<std::map<std::string, std::set<size_t>>,Err> sync_group(
            const std::string& group_id,
            const std::string& member_id,
            int generation_id,
            const std::map<std::string, std::map<std::string, std::set<size_t>>>& leader_assignments = {}
            ) ;


    HeartbeatResponce heartbeat(const std::string& group_id, const std::string& member_id) ;




private:
    std::unique_ptr<MessageQueue> pimpl;
};
#endif // MYMQ_S_H
