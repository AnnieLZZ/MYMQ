#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>

#include "MYMQ_Client.h"

static int test_stop_sweeps_callbacks() {
    std::atomic<int> called{0};
    std::atomic<int> wrong{0};

    MYMQ::Client::MYMQ_Produceruse producer("stop_semantics_test", 1);

    MYMQ_Public::TopicPartition tp("tp_stop_semantics_test", 0);
    auto cb = [&](MYMQ_Public::PushResponce resp) {
        called.fetch_add(1, std::memory_order_relaxed);
        if (resp.errorcode != MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED) {
            wrong.fetch_add(1, std::memory_order_relaxed);
        }
    };

    producer.inject_pending_callbacks_for_test(tp, 10, 5, cb);
    producer.stop();

    if (called.load(std::memory_order_relaxed) != 15) {
        std::cerr << "FAIL: stop callback count = " << called.load() << "\n";
        return 10;
    }
    if (wrong.load(std::memory_order_relaxed) != 0) {
        std::cerr << "FAIL: stop wrong errorcode = " << wrong.load() << "\n";
        return 11;
    }
    return 0;
}

static int test_destructor_on_exception_sweeps_callbacks() {
    std::atomic<int> called{0};
    std::atomic<int> wrong{0};

    MYMQ_Public::TopicPartition tp("tp_destructor_exception_test", 0);
    auto cb = [&](MYMQ_Public::PushResponce resp) {
        called.fetch_add(1, std::memory_order_relaxed);
        if (resp.errorcode != MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED) {
            wrong.fetch_add(1, std::memory_order_relaxed);
        }
    };

    try {
        MYMQ::Client::MYMQ_Produceruse producer("stop_semantics_test2", 1);
        producer.inject_pending_callbacks_for_test(tp, 7, 3, cb);
        throw std::runtime_error("forced");
    } catch (const std::exception&) {
    }

    if (called.load(std::memory_order_relaxed) != 10) {
        std::cerr << "FAIL: destructor callback count = " << called.load() << "\n";
        return 20;
    }
    if (wrong.load(std::memory_order_relaxed) != 0) {
        std::cerr << "FAIL: destructor wrong errorcode = " << wrong.load() << "\n";
        return 21;
    }
    return 0;
}

static int test_network_fatal_inflight_drain() {
    std::atomic<int> called{0};
    std::atomic<int> wrong{0};

    MYMQ::Network::Communication_client channel(MYMQ::run_directory_DEFAULT, MYMQ::REQUEST_TIMEOUT_MS_DEFAULT);

    auto cb = [&](uint16_t, std::vector<unsigned char> msg_body) {
        called.fetch_add(1, std::memory_order_relaxed);
        if (msg_body.size() < sizeof(uint16_t)) {
            wrong.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint16_t code_net = 0;
        std::memcpy(&code_net, msg_body.data(), sizeof(uint16_t));
        auto code = static_cast<MYMQ_Public::CommonErrorCode>(ntohs(code_net));
        if (code != MYMQ_Public::CommonErrorCode::NETWORK_FATAL) {
            wrong.fetch_add(1, std::memory_order_relaxed);
        }
    };



    channel.drain_inflight_with_error(MYMQ_Public::CommonErrorCode::NETWORK_FATAL);

    if (called.load(std::memory_order_relaxed) != 1) {
        std::cerr << "FAIL: inflight callback count = " << called.load() << "\n";
        return 31;
    }
    if (wrong.load(std::memory_order_relaxed) != 0) {
        std::cerr << "FAIL: inflight callback payload wrong = " << wrong.load() << "\n";
        return 32;
    }

    return 0;
}

int main() {
    if (int rc = test_stop_sweeps_callbacks(); rc != 0) return rc;
    if (int rc = test_destructor_on_exception_sweeps_callbacks(); rc != 0) return rc;
    if (int rc = test_network_fatal_inflight_drain(); rc != 0) return rc;

    std::cout << "PASS\n";
    return 0;
}

