#include"test_common_header.h"

#include <iomanip>
#include <sstream>
#include <chrono>
void cerr(const std::string& str){
    std::cout<<str<<std::endl;
}

void out(const std::string& str){
    std::cerr<<str<<std::endl;
}




// 辅助函数：获取当前模拟时间字符串
std::string getCurrentTimeStr() {
    // 这里简单模拟一个时间戳，实际项目可用 std::put_time
    static int sec_offset = 0;
    sec_offset++;
    int h = (10 + (sec_offset / 3600)) % 24;
    int m = (sec_offset / 60) % 60;
    int s = sec_offset % 60;

    std::stringstream ss;
    ss << "2025-12-04 "
       << std::setw(2) << std::setfill('0') << h << ":"
       << std::setw(2) << std::setfill('0') << m << ":"
       << std::setw(2) << std::setfill('0') << s;
    return ss.str();
}

// 辅助函数：从列表中随机选一个
std::string pickRandom(std::mt19937& rng, const std::vector<std::string>& list) {
    std::uniform_int_distribution<> dist(0, list.size() - 1);
    return list[dist(rng)];
}

// 1. 生成模拟日志行
// 格式: [TIME] [LEVEL] [MODULE] Message...
std::string generateLogLine(int targetLength, std::mt19937& rng) {
    const std::vector<std::string> levels = {"INFO", "DEBUG", "WARN", "ERROR", "FATAL"};
    const std::vector<std::string> modules = {"AuthSvc", "OrderDB", "PayGate", "UiRender", "NetWorker"};
    const std::vector<std::string> msgs = {
        "User login success", "Connection timed out", "Database query slow",
        "Payment processed", "Invalid token received", "Cache miss", "Retrying connection"
    };

    std::stringstream ss;
    ss << "[" << getCurrentTimeStr() << "] ";
    ss << "[" << pickRandom(rng, levels) << "] ";
    ss << "[" << pickRandom(rng, modules) << "] ";
    ss << pickRandom(rng, msgs);

    std::string base = ss.str();

    // 如果长度不够，补全 trace ID 或 padding
    if (base.length() < targetLength) {
        base += " | tid:";
        while (base.length() < targetLength) {
            base += std::to_string(rng() % 10);
        }
    }
    // 如果长度超出（一般保留截断或原样返回，这里选择截断以符合严格长度要求，但尽量保留头部）
    if (base.length() > targetLength) {
        base = base.substr(0, targetLength);
    }
    return base;
}

// 2. 生成模拟 JSON
// 格式: {"id": 123, "event": "...", "data": "..."}
std::string generateJson(int targetLength, std::mt19937& rng) {
    const std::vector<std::string> events = {"click", "view", "purchase", "heartbeat", "logout"};

    std::stringstream ss;
    ss << "{\"id\":" << (rng() % 100000) << ",";
    ss << "\"ts\":" << std::time(nullptr) << ",";
    ss << "\"type\":\"" << pickRandom(rng, events) << "\",";
    ss << "\"payload\":\"";

    std::string base = ss.str();
    // 计算还需要填充多少字符才能凑够 targetLength (预留最后 "})
    int paddingNeeded = targetLength - base.length() - 2;

    if (paddingNeeded > 0) {
        const char alphanum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::uniform_int_distribution<> dist(0, sizeof(alphanum) - 2);
        for(int i=0; i<paddingNeeded; ++i) {
            base += alphanum[dist(rng)];
        }
    }

    base += "\"}";
    return base;
}

// 3. 生成 CSV / 结构化记录
// 格式: UUID,UserId,Amount,Status,Region
std::string generateCSV(int targetLength, std::mt19937& rng) {
    const std::vector<std::string> status = {"PAID", "PENDING", "FAILED", "REFUNDED"};
    const std::vector<std::string> regions = {"CN-HZ", "CN-BJ", "US-WEST", "EU-CENTRAL"};

    std::stringstream ss;
    // UUID part (fake)
    ss << std::hex << rng() << rng() << "-" << std::dec;
    ss << (1000 + rng() % 9000) << ","; // UserID
    ss << (rng() % 1000) << "." << (rng() % 100) << ","; // Amount
    ss << pickRandom(rng, status) << ",";
    ss << pickRandom(rng, regions);

    std::string base = ss.str();
    // CSV 填充通常用 padding 字段
    if (base.length() < targetLength) {
        base += ",pad:";
        while(base.length() < targetLength) {
            base += 'X';
        }
    }
    if (base.length() > targetLength) {
        base = base.substr(0, targetLength);
    }
    return base;
}

// 4. 生成 URL / API 请求路径
// 格式: /api/v1/user/12345/profile?token=xyz...
std::string generateUrl(int targetLength, std::mt19937& rng) {
    const std::vector<std::string> resources = {"user", "order", "product", "image", "static"};
    const std::vector<std::string> actions = {"get", "update", "delete", "list", "sync"};

    std::stringstream ss;
    ss << "/api/v1/" << pickRandom(rng, resources) << "/"
       << (rng() % 10000) << "/" << pickRandom(rng, actions) << "?t=";

    std::string base = ss.str();
    // 填充 Token 参数
    if (base.length() < targetLength) {
        const char hex[] = "0123456789abcdef";
        std::uniform_int_distribution<> dist(0, 15);
        while(base.length() < targetLength) {
            base += hex[dist(rng)];
        }
    }
    return base;
}

// 5. IoT 传感器数据
// 格式: device_id=A101;temp=24.5;humid=60;vibration=0.02
std::string generateSensorData(int targetLength, std::mt19937& rng) {
    std::stringstream ss;
    ss << "dev=" << (rng()%100) << ";";
    ss << "T=" << (20 + (rng()%100)/10.0) << ";";
    ss << "H=" << (40 + (rng()%100)/2.0) << ";";
    ss << "V=" << (rng()%1000)/1000.0;

    std::string base = ss.str();
    // 填充
    if (base.length() < targetLength) {
        base += ";raw=";
        while(base.length() < targetLength) {
            base += std::to_string(rng() % 9);
        }
    }
    return base;
}

// ==========================================
// 主入口函数
// ==========================================

std::vector<std::string> generateRandomStringVector(
    int numStrings,
    int minLength,
    int maxLength,
    int mode // 现在代表数据模式 (1-5)

    ) {
    std::vector<std::string> stringVector;
    if (numStrings <= 0) return stringVector;

    stringVector.reserve(numStrings);

    // 种子生成
    std::mt19937 rng;
    try {
        std::random_device rd;
        rng.seed(rd());
    } catch (...) {
        rng.seed(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    }

    // 长度分布
    if (minLength <= 0) minLength = 10;
    if (maxLength < minLength) maxLength = minLength;
    std::uniform_int_distribution<> lengthDist(minLength, maxLength);

    // 确保 mode 在范围内
    int safeMode = mode;
    if (safeMode < 1) safeMode = 1;
    if (safeMode > 5) safeMode = 5;

    for (int i = 0; i < numStrings; ++i) {
        int currentLen = lengthDist(rng);
        std::string result;

        switch (safeMode) {
        case 1: result = generateLogLine(currentLen, rng); break;
        case 2: result = generateJson(currentLen, rng); break;
        case 3: result = generateCSV(currentLen, rng); break;
        case 4: result = generateUrl(currentLen, rng); break;
        case 5: result = generateSensorData(currentLen, rng); break;
        default: result = generateLogLine(currentLen, rng); break;
        }
        stringVector.emplace_back(std::move(result));
    }

    return stringVector;
}


