#include "test_common_header.h"
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>

// --- 性能测试配置 ---
const int NUM_MESSAGES = 4000000;      // 总测试消息量
const int MESSAGE_MIN_LEN = 1023;
const int MESSAGE_MAX_LEN = 1024;
const std::string TOPIC_NAME = "perf_test_topic";
const std::string GROUP_ID = "perf_group";

// --- 内存优化配置 ---
// true: 只生成 POOL_SIZE 条随机数据，发送时循环使用（节省内存，推荐）
// false: 生成 NUM_MESSAGES 条全量唯一数据（如果 NUM_MESSAGES 很大，可能占用 5GB+ 内存导致崩溃）
const bool USE_DATA_POOL = true;
const int POOL_SIZE = 100000;    // 随机数据池大小

using namespace std::chrono;

// 辅助函数：格式化吞吐量输出
void print_stats(const std::string& phase, double duration_sec, int total_msgs, long long total_bytes) {
    double ops = total_msgs / duration_sec;
    double mb_s = (total_bytes / 1024.0 / 1024.0) / duration_sec;

    std::cout << "\n[" << phase << " Results]" << std::endl;
    std::cout << "  Duration:   " << std::fixed << std::setprecision(3) << duration_sec << " s" << std::endl;
    std::cout << "  Count:      " << total_msgs << " messages" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << ops << " ops/sec" << std::endl;
    std::cout << "  Bandwidth:  " << std::fixed << std::setprecision(2) << mb_s << " MB/sec" << std::endl;
    std::cout << "-------------------------------------------" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "       MYMQ Performance Benchmark          " << std::endl;
    std::cout << "       (Random Payload / ZSTD Ready)       " << std::endl;
    std::cout << "===========================================" << std::endl;
    std::cout << "Target Messages: " << NUM_MESSAGES << std::endl;

    // --- 1. 数据准备阶段 ---
    int actual_gen_count = USE_DATA_POOL ? POOL_SIZE : NUM_MESSAGES;
    std::cout << "Preparing dataset (" << actual_gen_count << " items)... " << std::flush;

    // 生成随机 Value
    auto message_values = generateRandomStringVector(actual_gen_count, MESSAGE_MIN_LEN, MESSAGE_MAX_LEN,1);

    // 生成 Key
    std::vector<std::string> message_keys(actual_gen_count);
    for (int i = 0; i < actual_gen_count; ++i) {
        message_keys[i] = "key_" + std::to_string(i);
    }
    std::cout << "Done." << std::endl;

    // --- 2. 初始化客户端 ---
    MYMQ_Client mc("perf_client", 0);
    mc.create_topic(TOPIC_NAME);
    mc.subscribe_topic(TOPIC_NAME);
    mc.join_group(GROUP_ID);
    mc.set_local_pull_bytes_once(1048576000);

    // 等待连接建立
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // ==========================================
    // Phase 1: Producer Throughput (生产性能)
    // ==========================================
    out("\n--- [Phase 1] Producer Benchmark Start ---");

    auto start_push = high_resolution_clock::now();
    int push_count = 0;
    long long push_bytes = 0;

    for (int i = 0; i < NUM_MESSAGES; ++i) {
        // 如果使用数据池，用取模循环获取数据；否则直接通过下标获取
        int index = USE_DATA_POOL ? (i % POOL_SIZE) : i;

        // 使用引用 const string& 避免拷贝
        const std::string& key = message_keys[index];
        const std::string& val = message_values[index];

        MYMQ_Public::ClientErrorCode err = mc.push(
            MYMQ_Public::TopicPartition(TOPIC_NAME, 0),
            key,
            val
            );

        if (err != MYMQ_Public::ClientErrorCode::NULL_ERROR) {
            std::cerr << "Push error at " << i << ": " << MYMQ_Public::to_string(err) << std::endl;
        } else {
            push_count++;
            push_bytes += (key.size() + val.size());
        }

        if ((i + 1) % 500000 == 0) {
            std::cout << "Pushed " << (i + 1) << " / " << NUM_MESSAGES << "...\r" << std::flush;
        }
    }

    auto end_push = high_resolution_clock::now();
    duration<double> diff_push = end_push - start_push;
    std::cout << std::endl;
    print_stats("Producer", diff_push.count(), push_count, push_bytes);

    // 等待落盘/同步
    out("Waiting for server sync (5s)...");
    std::this_thread::sleep_for(std::chrono::seconds(20));


    mc.trigger_pull();
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    // ==========================================
    // Phase 2: Consumer Benchmark (Modified)
    // ==========================================
    out("\n--- [Phase 2] Consumer Benchmark Start ---");

    std::vector<MYMQ_Public::ConsumerRecord> res;

    int consumed_count = 0;
    long long consumed_bytes = 0;

    // 超时保护（Wall Clock）
    const int STALL_LIMIT_SEC = 50;
    auto last_data_time = high_resolution_clock::now();

    // 用于记录总墙钟时间（仅供参考）
    auto wall_clock_start = high_resolution_clock::now();

    // 【修改点1】定义累加器，用于存储 pull 返回的内部有效耗时
    // 单位为 微秒(us
    int64_t total_internal_cost_time = 0;

    while (consumed_count < NUM_MESSAGES) {
        res.clear();

        // 【修改点2】定义单次调用的耗时变量
        int64_t current_batch_cost_us = 0;

        // 【修改点3】调用 pull，传入引用接收实际耗时
        // 5000 是最大超时(ms)，current_batch_cost 将被赋值为实际处理时间
        auto err = mc.pull(res, 5000, current_batch_cost_us);

        if (!res.empty()) {
            // 只有成功拉取到数据，才计入有效吞吐量统计
            consumed_count += res.size();

            // 【修改点4】直接累加底层返回的有效时间
            total_internal_cost_time += current_batch_cost_us;

            for (const auto& msg : res) {
                consumed_bytes += msg.getValue_view().size() + msg.getKey_view().size();
            }

            // 更新活跃时间戳用于防死锁 (Wall Clock)
            last_data_time = high_resolution_clock::now();

            if (consumed_count % 500000 == 0) {
                std::cout << "Consumed " << consumed_count << " / " << NUM_MESSAGES << "...\r" << std::flush;
            }
        } else {
            // --- 空闲/超时分支 ---
            // 这里没有数据，意味着 current_batch_cost 可能是纯等待时间，
            // 对于"有效吞吐量"计算，通常不累加这段时间，或者根据你的定义决定是否累加。
            // 这里保持原逻辑：只统计处理数据的部分，因此不操作 total_internal_cost_time。

            // 错误检查
            if (err != MYMQ_Public::ClientErrorCode::PULL_TIMEOUT &&
                err != MYMQ_Public::ClientErrorCode::EMPTY_RECORD) {
                std::cerr << "\nCritical Pull Error: " << MYMQ_Public::to_string(err) << std::endl;
                break;
            }

            // 全局超时判断
            auto now = high_resolution_clock::now();
            if (duration_cast<std::chrono::seconds>(now - last_data_time).count() > STALL_LIMIT_SEC) {
                std::cerr << "\n[Timeout] Aborting." << std::endl;
                break;
            }
            std::this_thread::yield();
        }
    }

    std::cout << std::endl;

    double duration_sec = total_internal_cost_time / 1000000.0;

    // 防止除零
    if (duration_sec < 0.000001) duration_sec = 0.000001;

    // 额外打印总墙钟耗时供参考
    auto wall_clock_end = high_resolution_clock::now();
    duration<double> wall_diff = wall_clock_end - wall_clock_start;

    std::cout << "Total Wall Clock Time: " << wall_diff.count() << " s (Includes network wait)" << std::endl;
    std::cout << "Total Effective Processing Time: " << duration_sec << " s" << std::endl;

    // 打印吞吐量
    print_stats("Consumer (Effective)", duration_sec, consumed_count, consumed_bytes);

    return 0;
}
