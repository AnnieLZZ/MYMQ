#include<iostream>
#include<string>
#include"MYMQ_C.h"
#include"MYMQ_PublicCodes.h"
#include <string>
#include <queue>
#include <random>
#include <chrono> // For time-based seeding fallback
#include <stdexcept>
#include<memory>
#include<condition_variable>
#include<mutex>
#include "version.h"

void cerr(const std::string& str){
   std::cout<<str<<std::endl;
}

void out(const std::string& str){
    std::cerr<<str<<std::endl;
}

// void cerr(const std::string& str){
//     Printqueue::instance().out(str,1,0);
// }

// void out(const std::string& str){
//     Printqueue::instance().out(str,0,0);
// }
using Err_Client=MYMQ_Public::ClientErrorCode;

double getRepetitionProbability(int randomnessLevel) {
    switch (randomnessLevel) {
    case 1: return 0.9; // 很高几率重复前一个字符
    case 2: return 0.6; // 较高几率
    case 3: return 0.3; // 中等几率
    case 4: return 0.1; // 较低几率
    case 5: return 0.0; // 不重复前一个字符，完全随机选择
    default: return 0.0; // 默认最高随机性
    }
}

// generateRandomString 函数保持不变，因为它需要一个已初始化的 rng
std::string generateRandomString(int length, std::mt19937& rng, const std::string& charSet, int randomnessLevel) {
    if (charSet.empty()) {
        return ""; // 如果字符集为空，无法生成字符串
    }
    if (length <= 0) {
        return ""; // 长度为零或负数，返回空字符串
    }

    std::string randomString(length, ' ');
    std::uniform_int_distribution<> charDist(0, charSet.length() - 1);
    std::uniform_real_distribution<> probDist(0.0, 1.0); // 用于判断是否重复的概率分布

    double p_repeat = getRepetitionProbability(randomnessLevel);

    // 第一个字符总是从字符集中随机选择
    randomString[0] = charSet[charDist(rng)];

    // 从第二个字符开始，考虑重复概率
    for (int i = 1; i < length; ++i) {
        if (p_repeat > 0.0 && probDist(rng) < p_repeat) {
            // 如果满足重复条件，则重复前一个字符
            randomString[i] = randomString[i-1];
        } else {
            // 否则，从字符集中随机选择一个新字符
            randomString[i] = charSet[charDist(rng)];
        }
    }
    return randomString;
}

// 修改 generateRandomStringQueue 函数：
// - 移除 seed 参数
// - 将种子生成逻辑封装在函数内部
std::queue<std::string> generateRandomStringQueue(
    int numStrings,
    int minLength,
    int maxLength,
    int randomnessLevel, // 随机性级别参数 (1-5)，越高越随机
    const std::string& charSet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") {

    std::queue<std::string> stringQueue;

    if (numStrings <= 0) return stringQueue;
    if (minLength <= 0) minLength = 1;
    if (maxLength < minLength) maxLength = minLength;

    // 验证随机性级别，确保在 1 到 5 之间
    if (randomnessLevel < 1) randomnessLevel = 1;
    if (randomnessLevel > 5) randomnessLevel = 5;

    // 将种子生成逻辑封装在函数内部
    std::mt19937 rng;
    try {
        std::random_device rd;
        rng.seed(rd()); // 使用 std::random_device 作为种子
    } catch (const std::exception& e) {
        // 如果 random_device 不可用，则回退到时间作为种子
        rng.seed(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    }

    std::uniform_int_distribution<> lengthDist(minLength, maxLength);

    for (int i = 0; i < numStrings; ++i) {
        stringQueue.push(generateRandomString(lengthDist(rng), rng, charSet, randomnessLevel));
    }
    return stringQueue;
}


int main(){
      std::cout << "MYMQ Client: Current Version: " << MYMQ_VERSION_STRING << std::endl;
    // --- 吞吐量测试配置 ---
    const int NUM_MESSAGES_TO_TEST = 50000; // Number of messages to push and pull
    const int MESSAGE_MIN_LENGTH = 200;     // Minimum length of random message values
    const int MESSAGE_MAX_LENGTH = 300;    // Maximum length of random message values
    const std::string TOPIC_NAME = "testtopic";

    // -------------------------------------

    out("Generating " + std::to_string(NUM_MESSAGES_TO_TEST) + " random messages..."); // 正在生成 [NUM_MESSAGES_TO_TEST] 条随机消息...
    std::queue<std::string> message_values = generateRandomStringQueue(NUM_MESSAGES_TO_TEST, MESSAGE_MIN_LENGTH, MESSAGE_MAX_LENGTH,2);
    std::vector<std::string> message_keys(NUM_MESSAGES_TO_TEST);
    for (int i = 0; i < NUM_MESSAGES_TO_TEST; ++i) {
        message_keys[i] = "key_" + std::to_string(i);
    }
    out("Message generation complete."); // 消息生成完成。


    MYMQ_Client mc("test",2);
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
    // --- 推送吞吐量测试 ---
    out("\n--- Starting Push Throughput Test ---"); // 开始推送吞吐量测试
    auto push_start_time = std::chrono::high_resolution_clock::now();
    int messages_pushed = 0;
    for (int i = 0; i < NUM_MESSAGES_TO_TEST; ++i) {
        Err_Client err = mc.push(MYMQ_Public::TopicPartition(TOPIC_NAME,0),message_keys[i], message_values.front(),
                                 [](const MYMQ_Public::PushResponce& resp) {
            if( resp.errorcode!=MYMQ_Public::CommonErrorCode::NULL_ERROR){
                cerr(MYMQ_Public::to_string(resp.errorcode));
            }
                                 } );
        if (err != MYMQ_Public::ClientErrorCode:: NULL_ERROR) {
            cerr("Failed to push message " + std::to_string(i) + ", error code: " + std::to_string(static_cast<int>(err))); // 推送消息 [i] 失败，错误码: [err]
        } else {
            messages_pushed++;
        }
        message_values.pop(); // 消费队列中的值
    }
    auto push_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> push_duration = push_end_time - push_start_time;

    out("Push test complete."); // 推送测试完成。
    out("Total messages pushed: " + std::to_string(messages_pushed)); // 总共推送消息: [messages_pushed]
    out("Push duration: " + std::to_string(push_duration.count()) + " seconds"); // 推送耗时: [push_duration.count()] 秒
    if (push_duration.count() > 0) {
        out("Push throughput: " + std::to_string(messages_pushed / push_duration.count()) + " messages/second"); // 推送吞吐量: [throughput] 消息/秒
    } else {
        out("Push duration too short to calculate throughput."); // 推送耗时过短，无法计算吞吐量。
    }


    auto ass= mc.get_assigned_partition();
    for(const auto& [topic,partition]:ass){
        out(topic+" "+std::to_string(partition)+" Offset : "+std::to_string( mc.get_position_consumed(MYMQ_Public::TopicPartition(topic,partition))));
    }

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(5));
    }
    // --- 拉取和提交吞吐量测试 ---
    out("\n--- Starting Pull and Commit Throughput Test ---"); // 开始拉取和提交吞吐量测试
    int messages_pulled_count = 0;
    auto pull_start_time = std::chrono::steady_clock::now();

    bool first=0;
    while (messages_pulled_count < NUM_MESSAGES_TO_TEST) {

        auto pull_result = mc.pull(MYMQ_Public::TopicPartition(TOPIC_NAME,0));
        if(pull_result.second==Err_Client::PULL_TIMEOUT){
            out("pull timeout");
            continue;
        }
        else if (pull_result.second != Err_Client::NULL_ERROR && pull_result.first.empty()) {
            // 处理 NO_RECORD 或其他错误，但没有数据
            // 此时什么也不做，不提交，只继续循环
            continue;
        }

        if(!pull_result.first.empty()){
            // *** 只有在这里才启动计时器 ***
            if(!first){
                first=1;
                pull_start_time =std::chrono::steady_clock::now();
            }

            auto& msg_first=pull_result.first.front();
            auto& msg_back=pull_result.first.back();
            auto msg_num=pull_result.first.size();
            auto baseoffset=msg_first.getOffset();
            auto rearoffset=msg_back.getOffset();

            out("Batch : baseoffset= "+std::to_string(baseoffset)+" ,base_key ="+msg_first.getKey()+" ,rearoffset= "+std::to_string(rearoffset)+" ,rear_key= "+msg_back.getKey());
            messages_pulled_count+=msg_num;

            // *** 关键修改：在这里计算和提交 ***
            size_t next_offset_to_consume = rearoffset + 1 ;
            Err_Client commit_err = mc.commit_sync(MYMQ_Public::TopicPartition(TOPIC_NAME,0), next_offset_to_consume);

            if (commit_err != MYMQ_Public::ClientErrorCode:: NULL_ERROR) {
                cerr("Failed to commit offset " + std::to_string(next_offset_to_consume) + ", error code: " + std::to_string(static_cast<int>(commit_err)));
            }
        }
        // 如果 first.empty() == true (例如 NO_RECORD)，则此循环不执行任何操作，
        // 只会继续下一次 pull，而不会错误地提交 offset 1。
    }

    auto pull_end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> pull_duration = pull_end_time - pull_start_time;

    out("Pull and commit test complete."); // 拉取和提交测试完成。
    out("Total messages pulled: " + std::to_string(messages_pulled_count)); // 总共拉取消息: [messages_pulled_count]
    out("Pull and commit duration: " + std::to_string(pull_duration.count()) + " seconds"); // 拉取和提交耗时: [pull_duration.count()] 秒
    if (pull_duration.count() > 0) {
        out("Pull and commit throughput: " + std::to_string(messages_pulled_count / pull_duration.count()) + " messages/second"); // 拉取和提交吞吐量: [throughput] 消息/秒
    } else {
        out("Pull and commit duration too short to calculate throughput."); // 拉取和提交耗时过短，无法计算吞吐量。
    }

    out("\nThroughput test finished."); // 吞吐量测试结束。


    // mc.leave_group("testgroup");


    std::cin.get(); // 保持控制台打开
    return 0;
}













