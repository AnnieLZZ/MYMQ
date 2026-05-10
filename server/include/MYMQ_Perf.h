#ifndef MYMQ_PERF_H
#define MYMQ_PERF_H

#include <cstdint>

struct MYMQ_ServerPerfSnapshot {
    uint64_t total_requests = 0;
    uint64_t push_requests = 0;
    uint64_t push_success = 0;
    uint64_t push_failed = 0;
    uint64_t pull_requests = 0;
    uint64_t pull_hit = 0;
    uint64_t pull_no_record = 0;
    uint64_t commit_requests = 0;
    uint64_t commit_success = 0;
    uint64_t commit_failed = 0;
    uint64_t response_packets = 0;
    uint64_t response_file_packets = 0;
    uint64_t response_error_packets = 0;
    uint64_t pushed_payload_bytes = 0;
    uint64_t pulled_payload_bytes = 0;
    uint64_t pending_pull_requests = 0;
};

#endif // MYMQ_PERF_H
