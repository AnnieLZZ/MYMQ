#include "MYMQ_S.h"

#include "MessageQueue.h"
MYMQ_S::MYMQ_S()
    : pimpl(std::make_unique<MessageQueue>())
{

}

MYMQ_S::~MYMQ_S() = default;



