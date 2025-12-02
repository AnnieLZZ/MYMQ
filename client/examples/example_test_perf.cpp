#include "test_common_header.h"
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>

// --- 性能测试配置 ---
const int NUM_MESSAGES = 4000000;   // 测试消息总量 (例如 100万)
const int MESSAGE_SIZE = 1024;      // 单条消息大小 (1KB)，这是基准测试常用大小
const std::string TOPIC_NAME = "perf_test_topic";
const std::string GROUP_ID = "perf_group";

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
    std::cout << "===========================================" << std::endl;
    std::cout << "Messages: " << NUM_MESSAGES << std::endl;
    std::cout << "Msg Size: " << MESSAGE_SIZE << " bytes" << std::endl;

    // --- 1. 初始化 ---
    MYMQ_Client mc("perf_client", 0);
    mc.create_topic(TOPIC_NAME);
    mc.subscribe_topic(TOPIC_NAME);
    mc.join_group(GROUP_ID);

    // 预生成固定的 Payload，避免测试循环内的内存分配干扰结果
    std::string key_prefix = "k";
    std::string fixed_payload(MESSAGE_SIZE, 'x');

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
        // 使用简单的 key，减少格式化开销
        // 实际测试中，如果 MQ 对 Key 有特殊索引逻辑，这里的开销也要算在内
        std::string key = key_prefix + std::to_string(i);

        MYMQ_Public::ClientErrorCode err = mc.push(
            MYMQ_Public::TopicPartition(TOPIC_NAME, 0),
            key,
            fixed_payload
            );

        if (err != MYMQ_Public::ClientErrorCode::NULL_ERROR) {
            // 如果队列满了，可能需要简单的重试策略，或者直接报错
            // 这里为了简单，假设发送成功或直接报错
            std::cerr << "Push error at " << i << ": " << MYMQ_Public::to_string(err) << std::endl;
            // 生产环境测试可能需要 break，或者 sleep 重试
        } else {
            push_count++;
            push_bytes += (key.size() + fixed_payload.size());
        }

        if ((i + 1) % 100000 == 0) {
            std::cout << "Pushed " << (i + 1) << "..." << "\r" << std::flush;
        }
    }

    auto end_push = high_resolution_clock::now();
    duration<double> diff_push = end_push - start_push;

    print_stats("Producer", diff_push.count(), push_count, push_bytes);

    // 等待落盘/同步，模拟真实场景
    out("Waiting for server sync and consumer readiness (3s)...");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // ==========================================
    // Phase 2: Consumer Throughput (消费性能)
    // ==========================================
    out("\n--- [Phase 2] Consumer Benchmark Start ---");

    mc.trigger_pull();

    // 稍微给点时间让后台填充第一个 Batch，避免冷启动
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::vector<MYMQ_Public::ConsumerRecord> res;
    // 建议预留足够的空间，避免 vector 频繁扩容影响测试
    res.reserve(2000);

    int consumed_count = 0;
    long long consumed_bytes = 0;

    // 连续空拉取计数器
    int consecutive_empty_count = 0;
    const int MAX_EMPTY_RETRIES = 2; // 连续 10 次拉不到才算结束

    auto start_pull = high_resolution_clock::now();
    auto last_success_time = start_pull; // [关键] 记录最后一次拿到数据的时间

    while (true) {
        res.clear();

        // 使用 0 超时 (Non-blocking)，尽可能压榨 CPU
        auto err = mc.pull(res, 0);

        if (!res.empty()) {
            // === 有数据 ===
            consumed_count += res.size();

            // 简单统计字节数 (模拟业务处理)
            for (const auto& msg : res) {
                consumed_bytes += msg.getValue().size() + msg.getKey().size();
            }

            // [关键] 更新最后成功时间
            last_success_time = high_resolution_clock::now();

            // 重置重试计数器
            consecutive_empty_count = 0;

            // 打印进度
            if (consumed_count % 100000 == 0) {
                std::cout << "Consumed " << consumed_count << "...\r" << std::flush;
            }
        } else {
            // === 没数据 ===
            // 只有当 err 确实是没数据时才计数
            if (err == MYMQ_Public::ClientErrorCode::PULL_TIMEOUT ||
                err == MYMQ_Public::ClientErrorCode::EMPTY_RECORD) {

                consecutive_empty_count++;

                // 如果连续 N 次都没拉到数据，且我们已经拉取了一定量的数据，认为消费结束
                if (consecutive_empty_count >= MAX_EMPTY_RETRIES) {
                    if (consumed_count > 0) {
                        std::cout << "\nqueue drained. stopping benchmark." << std::endl;
                    } else {
                        std::cerr << "\nwarning: no data consumed at all!" << std::endl;
                    }
                    break;
                }

                // 可选：稍微 yield 一下 CPU，给后台线程一点机会填数据
                // std::this_thread::yield();
            } else {
                // 发生了其他错误（如解析失败），视情况处理
                std::cerr << "pull error: " << MYMQ_Public::to_string(err) << std::endl;
                break;
            }
        }
    }

    // [关键修正] 计算耗时使用 last_success_time，剔除最后几次空转的时间
    duration<double> diff_pull = last_success_time - start_pull;

    // 防止除以0（如果完全没拉到数据）
    if (diff_pull.count() < 0.000001) diff_pull = duration<double>(0.000001);

    print_stats("Consumer", diff_pull.count(), consumed_count, consumed_bytes);

    std::cout << "Benchmark Complete." << std::endl;
    return 0;
}
