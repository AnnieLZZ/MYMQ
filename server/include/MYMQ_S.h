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


private:
    std::unique_ptr<MessageQueue> pimpl;
};
#endif // MYMQ_S_H
