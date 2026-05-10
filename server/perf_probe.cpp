#include "MYMQ_S.h"
#include "version.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_running{true};

void on_sigint(int) {
    g_running.store(false, std::memory_order_release);
}

uint64_t safe_sub(uint64_t now, uint64_t prev) {
    return (now >= prev) ? (now - prev) : 0;
}
} // namespace

int main() {
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    std::cout << "[MYMQ PERF PROBE] Server Version: " << SERVER_VERSION_STRING << std::endl;
    std::cout << "[MYMQ PERF PROBE] Starting broker..." << std::endl;

    MYMQ_S broker;
    broker.reset_perf_counters();

    auto last = broker.get_perf_snapshot();
    std::cout << "[MYMQ PERF PROBE] Sampling every 1 second. Press Ctrl+C to stop." << std::endl;

    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = broker.get_perf_snapshot();

        const uint64_t req_ps = safe_sub(now.total_requests, last.total_requests);
        const uint64_t push_ps = safe_sub(now.push_requests, last.push_requests);
        const uint64_t push_ok_ps = safe_sub(now.push_success, last.push_success);
        const uint64_t pull_ps = safe_sub(now.pull_requests, last.pull_requests);
        const uint64_t pull_hit_ps = safe_sub(now.pull_hit, last.pull_hit);
        const uint64_t commit_ps = safe_sub(now.commit_requests, last.commit_requests);
        const uint64_t commit_ok_ps = safe_sub(now.commit_success, last.commit_success);
        const uint64_t in_bytes_ps = safe_sub(now.pushed_payload_bytes, last.pushed_payload_bytes);
        const uint64_t out_bytes_ps = safe_sub(now.pulled_payload_bytes, last.pulled_payload_bytes);

        std::cout
            << "[1s] req=" << req_ps
            << " push=" << push_ps << "(ok=" << push_ok_ps << ")"
            << " pull=" << pull_ps << "(hit=" << pull_hit_ps << ")"
            << " commit=" << commit_ps << "(ok=" << commit_ok_ps << ")"
            << " in_bytes=" << in_bytes_ps
            << " out_bytes=" << out_bytes_ps
            << " pending_pull=" << now.pending_pull_requests
            << std::endl;

        last = now;
    }

    std::cout << "[MYMQ PERF PROBE] Stopping..." << std::endl;
    return 0;
}

