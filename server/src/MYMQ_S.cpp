#include "MYMQ_S.h"

#include "MessageQueue.h"
MYMQ_S::MYMQ_S()
    : pimpl(std::make_unique<MessageQueue>())
{

}

MYMQ_S::~MYMQ_S() = default;

MYMQ_ServerPerfSnapshot MYMQ_S::get_perf_snapshot() const {
    return pimpl->get_perf_snapshot();
}

void MYMQ_S::reset_perf_counters() {
    pimpl->reset_perf_counters();
}



