#include"test_common_header.h"
void cerr(const std::string& str){
    // std::cout<<str<<std::endl;
}

void out(const std::string& str){
    // std::cerr<<str<<std::endl;
}

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
std::vector<std::string> generateRandomStringVector(
    int numStrings,
    int minLength,
    int maxLength,
    int randomnessLevel, // 随机性级别参数 (1-5)
    const std::string& charSet) {

    std::vector<std::string> stringVector;

    if (numStrings <= 0) return stringVector;

    // 【优化】预分配内存，避免 vector 扩容带来的性能损耗
    stringVector.reserve(numStrings);

    if (minLength <= 0) minLength = 1;
    if (maxLength < minLength) maxLength = minLength;

    // 验证随机性级别
    if (randomnessLevel < 1) randomnessLevel = 1;
    if (randomnessLevel > 5) randomnessLevel = 5;

    // 种子生成逻辑
    std::mt19937 rng;
    try {
        std::random_device rd;
        rng.seed(rd());
    } catch (const std::exception& e) {
        rng.seed(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    }

    std::uniform_int_distribution<> lengthDist(minLength, maxLength);

    for (int i = 0; i < numStrings; ++i) {
        // 使用 emplace_back 直接在 vector 尾部构造
        stringVector.emplace_back(generateRandomString(lengthDist(rng), rng, charSet, randomnessLevel));
    }

    return stringVector;
}
