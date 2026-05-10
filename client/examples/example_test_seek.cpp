#include "test_common_header.h"
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <cassert>

using Err_Client = MYMQ_Public::ClientErrorCode;

// 用于本地存储预期的消息结构
struct ExpectedMessage {
    std::string key;
    std::string value;
};

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "    MYMQ Data Integrity & Correctness Test  " << std::endl;
    std::cout << "      (Producer / Consumer Separated)       " << std::endl;
    std::cout << "===========================================" << std::endl;
    std::cout << "Client Version: " << CLIENT_VERSION_STRING << std::endl;

    // --- 配置参数 ---
    const int NUM_MESSAGES_TO_VERIFY = 50000;
    const int MESSAGE_LEN = 100;
    const std::string TOPIC_NAME = "verify_topic";
    const std::string GROUP_ID = "verify_group";
    const size_t PULL_TIMEOUT_S = 2;

    // --- 1. 生成并记录预期数据 ---
    // [逻辑不变] 生成确定的数据，方便比对
    out("Generating " + std::to_string(NUM_MESSAGES_TO_VERIFY) + " verification messages...");

    std::vector<ExpectedMessage> expected_data;
    expected_data.reserve(NUM_MESSAGES_TO_VERIFY);

    for (int i = 0; i < NUM_MESSAGES_TO_VERIFY; ++i) {
        ExpectedMessage msg;
        msg.key = "key_" + std::to_string(i);
        // 为了确保内容独一无二，将 index 编码进 value
        msg.value = "val_" + std::to_string(i) + "_" + std::string(MESSAGE_LEN, 'x');
        expected_data.push_back(msg);
    }
    out("Data generation complete.");

    // --- 2. 初始化客户端与生产者 (Refactored) ---

    // [修改点] 初始化生产者 mp，负责建Topic和发消息
    MYMQ_Producer mp("verify_producer", 1);
    mp.create_topic(TOPIC_NAME);

    // [修改点] 初始化消费者 mc，负责订阅和拉取
    MYMQ_Consumer mc("verify_consumer", 0);
    mc.subscribe_topic(TOPIC_NAME);
    mc.join_group(GROUP_ID);
    // 校验测试通常不需要设置特别大的缓冲区，默认即可，或者根据需要设置
    // mc.set_local_pull_bytes_once(1024 * 1024);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // --- 3. 推送数据 (Phase 1: Producer) ---
    out("\n--- [Phase 1] Pushing Messages (Producer) ---");
    int push_success_count = 0;

    for (int i = 0; i < NUM_MESSAGES_TO_VERIFY; ++i) {
        // [修改点] 使用 mp (Producer) 发送消息，替代原来的 mc.push
        Err_Client err = mp.push(
            MYMQ_Public::TopicPartition(TOPIC_NAME, 0),
            expected_data[i].key,
            expected_data[i].value
            );

        if (err != Err_Client::Success) {
            cerr("FATAL: Push failed at index " + std::to_string(i) + " Error: " + MYMQ_Public::to_string(err));
            return -1; // 发送失败直接退出，验证测试要求 100% 可靠
        }
        push_success_count++;

        if ((i + 1) % 10000 == 0) {
            std::cout << "Pushed " << (i + 1) << " / " << NUM_MESSAGES_TO_VERIFY << "\r" << std::flush;
        }
    }
    std::cout << "\nPush Complete. Success: " << push_success_count << std::endl;

    // 等待数据落盘/同步
    out("Waiting for server sync (5s)...");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // --- 4. 拉取并校验数据 (Phase 2: Consumer) ---
    out("\n--- [Phase 2] Verifying Messages (Consumer) ---");

    // [修改点] 触发消费者的拉取预热
    mc.trigger_pull();
    std::this_thread::sleep_for(std::chrono::seconds(3)); // 等待消息进入缓冲区

    std::vector<MYMQ_Public::ConsumerRecord> res;
    res.reserve(2000);

    int verified_count = 0;
    bool verification_passed = true;
    int retry_empty_count = 0; // 防止无限等待

    // [逻辑不变] 核心校验循环
    while (verified_count < NUM_MESSAGES_TO_VERIFY) {
        res.clear();

        // 用于接收耗时参数（虽然校验测试不关注耗时，但API需要）
        int64_t dummy_cost_us = 0;

        // [修改点] 这里如果你的 pull 接口有多个重载，请适配上面的 Performance 代码
        // 如果 pull 原型是 pull(vector&, timeout)，保持原样：
        auto pull_result = mc.pull(res, PULL_TIMEOUT_S);
        // 如果 pull 原型是 pull(vector&, timeout, cost_us)，则改为：
        // auto pull_result = mc.pull(res, PULL_TIMEOUT_S * 1000, dummy_cost_us);

        // 调试日志：如果拉取为空，打印状态
        if (res.empty() && pull_result != Err_Client::Success) {
            // 简单的 debug 输出，防止大量刷屏，仅在非预期错误时打印
            if (pull_result != Err_Client::PULL_TIMEOUT && pull_result != Err_Client::EMPTY_RECORD) {
                std::cout << "[DEBUG] pull_result=" << MYMQ_Public::to_string(pull_result) << std::endl;
            }
        }

        if (pull_result == Err_Client::PULL_TIMEOUT || pull_result == Err_Client::EMPTY_RECORD) {
            retry_empty_count++;
            if (retry_empty_count > 15) { // 稍微放宽一点重试次数
                cerr("TIMEOUT: Server stopped sending data. Stuck at index " + std::to_string(verified_count));
                verification_passed = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 避免空转太快
            continue;
        }

        if (pull_result != Err_Client::Success && pull_result != Err_Client::PARTIAL_PARASE_FAILED) {
            cerr("ERROR: Pull failed with code: " + MYMQ_Public::to_string(pull_result));
            verification_passed = false;
            break;
        }

        retry_empty_count = 0; // 重置超时计数

        // === 核心校验逻辑 (逐条比对) ===
        for (const auto& record : res) {
            // 获取当前期望的数据
            if (verified_count >= expected_data.size()) {
                cerr("ERROR: Received more messages than expected!");
                verification_passed = false;
                goto end_verification;
            }

            const auto& expected = expected_data[verified_count];

            std::string actual_key = record.getKey();
            std::string actual_val = record.getValue();

            // 1. 校验 Key
            if (actual_key != expected.key) {
                cerr("\n[FAIL] Key Mismatch at index " + std::to_string(verified_count));
                cerr("Expected: " + expected.key);
                cerr("Actual:   " + actual_key);
                cerr("Offset:   " + std::to_string(record.getOffset()));
                verification_passed = false;
                goto end_verification;
            }

            // 2. 校验 Value
            if (actual_val != expected.value) {
                cerr("\n[FAIL] Value Mismatch at index " + std::to_string(verified_count));
                cerr("Expected (len=" + std::to_string(expected.value.size()) + "): " + expected.value.substr(0, 50) + "...");
                cerr("Actual   (len=" + std::to_string(actual_val.size()) + "): " + actual_val.substr(0, 50) + "...");
                verification_passed = false;
                goto end_verification;
            }

            verified_count++;
        }

        // 打印进度
        if (verified_count % 5000 == 0) {
            std::cout << "Verified " << verified_count << " / " << NUM_MESSAGES_TO_VERIFY << "\r" << std::flush;
        }
    }

end_verification:
    std::cout << "\n\n";
    if (verification_passed && verified_count == NUM_MESSAGES_TO_VERIFY) {
        std::cout << "========================================" << std::endl;
        std::cout << " TEST RESULT: [ PASS ]" << std::endl;
        std::cout << " Verified " << verified_count << " messages successfully." << std::endl;
        std::cout << " Data Integrity: OK" << std::endl;
        std::cout << " Order Integrity: OK" << std::endl;
        std::cout << "========================================" << std::endl;
    } else {
        std::cout << "========================================" << std::endl;
        std::cout << " TEST RESULT: [ FAILED ]" << std::endl;
        std::cout << " Verified only " << verified_count << " / " << NUM_MESSAGES_TO_VERIFY << std::endl;
        std::cout << "========================================" << std::endl;
    }

    std::cin.get();
    return verification_passed ? 0 : 1;
}
