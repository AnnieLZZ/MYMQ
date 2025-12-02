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
    std::cout << "   MYMQ Data Integrity & Correctness Test  " << std::endl;
    std::cout << "===========================================" << std::endl;
    std::cout << "Client Version: " << CLIENT_VERSION_STRING << std::endl;

    // --- 配置参数 ---
    // 验证测试不需要像吞吐量测试那么大的量，重点是“准确”
    const int NUM_MESSAGES_TO_VERIFY = 50000;
    const int MESSAGE_LEN = 100; // 固定长度方便对比，或者随机长度均可
    const std::string TOPIC_NAME = "verify_topic";
    const std::string GROUP_ID = "verify_group";
    const size_t PULL_TIMEOUT_S = 2;

    // --- 1. 生成并记录预期数据 ---
    out("Generating " + std::to_string(NUM_MESSAGES_TO_VERIFY) + " verification messages...");

    std::vector<ExpectedMessage> expected_data;
    expected_data.reserve(NUM_MESSAGES_TO_VERIFY);

    // 生成确定的数据，方便调试。如果出错，我们知道 key_100 就应该对应 index 100
    for (int i = 0; i < NUM_MESSAGES_TO_VERIFY; ++i) {
        ExpectedMessage msg;
        msg.key = "key_" + std::to_string(i);
        // 为了确保内容独一无二，将 index 编码进 value
        msg.value = "val_" + std::to_string(i) + "_" + std::string(MESSAGE_LEN, 'x');
        expected_data.push_back(msg);
    }
    out("Data generation complete.");

    // --- 2. 初始化客户端 ---
    MYMQ_Client mc("verify_client", 1);
    // 确保 Topic 是空的或者新创建的，防止读取到脏数据
    // 注意：在真实测试中，最好每次跑测试都用不同的 Topic 名字，或者先清空 Topic
    mc.subscribe_topic(TOPIC_NAME);
    mc.create_topic(TOPIC_NAME); 
    mc.join_group(GROUP_ID);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // --- 3. 推送数据 (Producer Phase) ---
    out("\n--- [Phase 1] Pushing Messages ---");
    int push_success_count = 0;

    for (int i = 0; i < NUM_MESSAGES_TO_VERIFY; ++i) {
        Err_Client err = mc.push(
            MYMQ_Public::TopicPartition(TOPIC_NAME, 0),
            expected_data[i].key,
            expected_data[i].value
            );

        if (err != Err_Client::NULL_ERROR) {
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
    out("Waiting for server sync (3s)...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // --- 4. 拉取并校验数据 (Consumer Phase) ---
    out("\n--- [Phase 2] Verifying Messages ---");
    mc.trigger_pull();
    std::this_thread::sleep_for(std::chrono::seconds(3));//trigger完等一会让消息进入缓冲区

    std::vector<MYMQ_Public::ConsumerRecord> res;
    res.reserve(2000);

    int verified_count = 0;
    bool verification_passed = true;
    int retry_empty_count = 0; // 防止无限等待

    while (verified_count < NUM_MESSAGES_TO_VERIFY) {
        res.clear();
        auto pull_result = mc.pull(res, PULL_TIMEOUT_S);

        std::cout << "[DEBUG] pull_result=" << MYMQ_Public::to_string(pull_result)
                  << ", res.size()=" << res.size() << std::endl;
        if (pull_result == Err_Client::PULL_TIMEOUT || pull_result == Err_Client::EMPTY_RECORD) {
            retry_empty_count++;
            if (retry_empty_count > 10) {
                cerr("TIMEOUT: Server stopped sending data. Stuck at index " + std::to_string(verified_count));
                verification_passed = false;
                break;
            }
            continue;
        }

        if (pull_result != Err_Client::NULL_ERROR && pull_result != Err_Client::PARTIAL_PARASE_FAILED) {
            cerr("ERROR: Pull failed with code: " + MYMQ_Public::to_string(pull_result));
            verification_passed = false;
            break;
        }

        retry_empty_count = 0; // 重置超时计数

        // === 核心校验逻辑 ===
        for (const auto& record : res) {
            // 获取当前期望的数据
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
                // 只打印前50个字符避免刷屏
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
