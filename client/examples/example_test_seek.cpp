
#include"test_common_header.h"


// void cerr(const std::string& str){
//     Printqueue::instance().out(str,1,0);
// }

// void out(const std::string& str){
//     Printqueue::instance().out(str,0,0);
// }
using Err_Client=MYMQ_Public::ClientErrorCode;




int main(){
    std::cout << "MYMQ Client: Current Version: " << CLIENT_VERSION_STRING << std::endl;
    // --- 吞吐量测试配置 ---
    const int NUM_MESSAGES_TO_TEST = 10000; // Number of messages to push and pull
    const int MESSAGE_MIN_LENGTH = 200;     // Minimum length of random message values
    const int MESSAGE_MAX_LENGTH = 300;    // Maximum length of random message values
    const std::string TOPIC_NAME = "testtopic";

    // -------------------------------------

    out("Generating " + std::to_string(NUM_MESSAGES_TO_TEST) + " random messages..."); // 正在生成 [NUM_MESSAGES_TO_TEST] 条随机消息...
    auto message_values = generateRandomStringVector(NUM_MESSAGES_TO_TEST, MESSAGE_MIN_LENGTH, MESSAGE_MAX_LENGTH,2);
    std::vector<std::string> message_keys(NUM_MESSAGES_TO_TEST);
    for (int i = 0; i < NUM_MESSAGES_TO_TEST; ++i) {
        message_keys[i] = "key_" + std::to_string(i);
    }
    out("Message generation complete."); // 消息生成完成。


    MYMQ_Client mc("test",1);
    mc.subscribe_topic("testtopic");
    mc.create_topic("testtopic");
    mc.join_group("testgroup");
    mc.set_pull_bytes(1024*1024);


    std::condition_variable cv;
    std::mutex mtx;


    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(2));
    }

    auto ass= mc.get_assigned_partition();
    out("\n--- Your assignment ---");
    for(const auto& [topic,partition]:ass){
        out(topic+" "+std::to_string(partition)+" Offset : "+std::to_string( mc.get_position_consumed(MYMQ_Public::TopicPartition(topic,partition))));
    }
    out("\n--- Your assignment ---");

    int messages_pushed = 0;
    for (int i = 0; i < NUM_MESSAGES_TO_TEST; ++i) {
        Err_Client err = mc.push(MYMQ_Public::TopicPartition(TOPIC_NAME,0),message_keys[i], message_values[i] );

        if (err != MYMQ_Public::ClientErrorCode:: NULL_ERROR) {
            cerr("Failed to push message " + std::to_string(i) + ", error code: " + std::to_string(static_cast<int>(err))); // 推送消息 [i] 失败，错误码: [err]
        } else {
            messages_pushed++;
        }
    }





    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(10));
    }
    // --- Seek 功能正确性测试 ---
    out("\n========================================");
    out("      Starting Seek Capability Test       ");
    out("========================================");

    MYMQ_Public::TopicPartition tp(TOPIC_NAME, 0);

    // 辅助 lambda：拉取并打印第一条消息的 Offset 和 Key
    auto pull_and_check = [&](const std::string& step_name, size_t expected_offset) {
        std::vector<MYMQ_Public::ConsumerRecord> res;
        out("\n[" + step_name + "] Pulling data...");

        auto err = mc.pull( res);

        if (err != Err_Client::NULL_ERROR && res.empty()) {
            cerr(">> FAILED: Pull returned error or empty. Code: " + std::to_string(static_cast<int>(err)));
            return;
        }

        if (res.empty()) {
            cerr(">> FAILED: Result set is empty!");
            return;
        }

        out("   Fetched batch size: " + std::to_string(res.size()));
        out("   First Message in Batch -> Offset: " + std::to_string(res.front().getOffset()));

        // --- 核心修改开始 ---
        // 我们不能假设 res.front() 就是我们要找的 offset，因为可能拉回了包含该 offset 的整个批次
        // 我们需要在 res 中遍历查找
        bool target_found = false;
        for (const auto& msg : res) {
            if (msg.getOffset() == expected_offset) {
                out(">> SUCCESS: Found expected Offset (" + std::to_string(expected_offset) + ") in the fetched batch.");
                out("   Key match check: " + msg.getKey());
                target_found = true;
                break; // 找到了就可以跳出
            }
            else{
                std::cout<<std::to_string(msg.getOffset());

            }
        }
        std::cout<<std::endl;

        if (!target_found) {
            // 如果遍历完了还没找到，说明虽然拉到了数据，但数据范围不包含我们要的 offset
            // (例如：我们要 50，结果拉回了 0-40，或者 60-100)
            cerr(">> FAILED: Expected Offset " + std::to_string(expected_offset) +
                 " NOT found in fetched batch (Range: " +
                 std::to_string(res.front().getOffset()) + " - " +
                 std::to_string(res.back().getOffset()) + ")");
        }
        // --- 核心修改结束 ---
    };
    // -------------------------------------------------
    // Case 1: 正常拉取（Sanity Check）
    // -------------------------------------------------
    // 假设 Push 成功，当前应该能拉到 0 位置（或者消费者组之前的进度，这里假设是新组从头开始）
    // 为了测试 seek，我们先随便拉一下，消耗一些 offset
    out("\n--- Case 1: Initial Normal Pull ---");
    std::vector<MYMQ_Public::ConsumerRecord> dummy_res;
    mc.pull( dummy_res);
    if (!dummy_res.empty()) {
        out("Consumed initial batch from " + std::to_string(dummy_res.front().getOffset()) +
            " to " + std::to_string(dummy_res.back().getOffset()));
        // 提交一下，模拟正常消费进度推进
        mc.commit_sync(tp, dummy_res.back().getOffset() + 1);
    } else {
        out("Initial pull empty (maybe acceptable depending on retention), proceeding to explicit seeks.");
    }

    // -------------------------------------------------
    // Case 2: Seek 回溯到起点 (Seek to Beginning)
    // -------------------------------------------------
    out("\n--- Case 2: Seek back to Offset 0 ---");
    mc.seek(tp, 0); // <--- 调用 Seek
    pull_and_check("Verify Seek(0)", 0);

    // -------------------------------------------------
    // Case 3: Seek 到中间特定位置 (Seek to Specific Offset)
    // -------------------------------------------------
    // 假设我们要跳到第 50 条消息
    size_t target_mid = 50;
    if (NUM_MESSAGES_TO_TEST > target_mid) {
        out("\n--- Case 3: Seek forward/random to Offset " + std::to_string(target_mid) + " ---");
        mc.seek(tp, target_mid);

        std::vector<MYMQ_Public::ConsumerRecord> res_mid;
        mc.pull( res_mid);

        if (!res_mid.empty()) {
            std::string expected_key = "key_" + std::to_string(target_mid);
            bool found = false;

            // --- 修改：遍历查找 ---
            for (const auto& msg : res_mid) {
                if (msg.getOffset() == target_mid) {
                    found = true;
                    if (msg.getKey() == expected_key) {
                        out(">> SUCCESS: Target Offset " + std::to_string(target_mid) + " found and Key matches.");
                    } else {
                        cerr(">> FAILED: Offset matched but Key mismatch. Got " + msg.getKey() + ", expected " + expected_key);
                    }
                    break;
                }
            }

            if (!found) {
                cerr(">> FAILED: Seek target " + std::to_string(target_mid) + " not found in returned batch. " +
                     "Batch starts at " + std::to_string(res_mid.front().getOffset()));
            }
            // ---------------------

        } else {
            cerr(">> FAILED: Pull returned empty after seek.");
        }
    }

    // -------------------------------------------------
    // Case 4: Seek 到末尾附近 (Seek near End)
    // -------------------------------------------------
    size_t target_end = NUM_MESSAGES_TO_TEST - 5;
    if (NUM_MESSAGES_TO_TEST > 10) {
        out("\n--- Case 4: Seek to near end Offset " + std::to_string(target_end) + " ---");
        mc.seek(tp, target_end); // <--- 调用 Seek
        pull_and_check("Verify Seek(End)", target_end);
    }

    out("\nSeek functionality test finished.");

    std::cin.get(); // 保持控制台打开
    return 0;
}


