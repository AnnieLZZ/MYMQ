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
    auto message_values = generateRandomStringVector(actual_gen_count, MESSAGE_MIN_LEN, MESSAGE_MAX_LEN, 2);

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
    std::this_thread::sleep_for(std::chrono::seconds(5));


    mc.trigger_pull();
    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    // ==========================================
    // Phase 2: Consumer Throughput (定量消费)
    // ==========================================
    out("\n--- [Phase 2] Consumer Benchmark Start ---");

    std::vector<MYMQ_Public::ConsumerRecord> res;

    int consumed_count = 0;
    long long consumed_bytes = 0;

    // 超时保护
    const int STALL_LIMIT_SEC = 30;
    auto last_data_time = high_resolution_clock::now();

    // 【修改点1】不再使用单一的 start/end，而是使用累积时间
    std::chrono::duration<double> total_active_duration(0);

    // 用于超时判断的墙钟时间（Wall Clock）依旧需要保留
    auto wall_clock_start = high_resolution_clock::now();

    while (consumed_count < NUM_MESSAGES) {
        res.clear();

        // 【修改点2】记录本次操作的开始时间
        auto batch_start_time = high_resolution_clock::now();

        // 执行拉取
        auto err = mc.pull(res, 2000);

        // 【修改点3】判断是否有数据
        if (!res.empty()) {
            // 只有获取到了数据，才进行统计和计时累加
            consumed_count += res.size();

            for (const auto& msg : res) {
                consumed_bytes += msg.getValue_view().size() + msg.getKey_view().size();
            }

            // 记录本次操作结束时间
            auto batch_end_time = high_resolution_clock::now();

            // 【关键】：只累加这一段有效工作的时间
            total_active_duration += (batch_end_time - batch_start_time);

            // 更新活跃时间戳用于防死锁
            last_data_time = high_resolution_clock::now();

            if (consumed_count % 500000 == 0) {
                // 打印进度时，可以用 wall clock 算一个大概的进度感，或者只打印数量
                std::cout << "Consumed " << consumed_count << " / " << NUM_MESSAGES << "...\r" << std::flush;
            }
        } else {
            // --- 这里是空闲分支 ---
            // 这里的耗时完全不计入 total_active_duration

            // 错误检查与超时处理 (逻辑不变)
            if (err != MYMQ_Public::ClientErrorCode::PULL_TIMEOUT &&
                err != MYMQ_Public::ClientErrorCode::EMPTY_RECORD) {
                std::cerr << "\nCritical Pull Error: " << MYMQ_Public::to_string(err) << std::endl;
                break;
            }
            auto now = high_resolution_clock::now();
            if (duration_cast<std::chrono::seconds>(now - last_data_time).count() > STALL_LIMIT_SEC) {
                std::cerr << "\n[Timeout] Aborting." << std::endl;
                break;
            }
            // 避免空转占满 CPU，适当 yield，因为这里的时间已经被剔除了，所以 yield 不会影响你的吞吐量成绩
            std::this_thread::yield();
        }
    }

    std::cout << std::endl;

    // 【修改点4】计算使用累积时间，防止时间为0导致除零异常
    double duration_sec = total_active_duration.count();
    if (duration_sec < 0.000001) duration_sec = 0.000001;

    // 额外打印总墙钟耗时供参考（可选）
    auto wall_clock_end = high_resolution_clock::now();
    duration<double> wall_diff = wall_clock_end - wall_clock_start;
    std::cout << "Total Wall Clock Time: " << wall_diff.count() << " s (Includes idle wait)" << std::endl;

    // 打印只包含有效处理时间的吞吐量
    print_stats("Consumer (Effective)", duration_sec, consumed_count, consumed_bytes);

    return 0;
}
