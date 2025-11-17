# MYMQ

> (Learning Project) A high-performance C++ MQ built from scratch, inspired by Kafka.
> 
> (学习项目) 一个从零构建的、受 Kafka 启发的 C++ 高性能消息队列。

## 🌟 核心特性 (Features)

* **跨平台 (Cross-Platform):** 服务器端 (Linux) 和 客户端 (Windows)
* **极高性能 (Blazing Fast):** 专为 C++ 优化，单分区 Push 吞吐量平均可达 100,000 msg/s 左右，Pull 吞吐量平均 95,000 msg/s 左右 (消息平均长度 200-300 字节)。
* **单节点架构 (Single-Node):** 实现了 Kafka 的核心概念（如分区、持久化日志），但摒弃了分布式的复杂性，实现了极低的延迟。
* **消息完整性 (Data Integrity):** 使用 `zlib::crc32` 校验，确保消息在传输和存储过程中不被损坏。

## 🛠️ 技术栈 (Technology Stack)

* **并发 (Concurrency):** `Intel TBB`
* **无锁队列 (Lock-Free Queue):** `moodycamel::ReadWriterQueue`
* **压缩 (Compression):** `zstd`
* **校验 (Checksum):** `zlib::crc32`

## 📖 如何使用 (How to Use)

**关于如何调用 API (例如 Push/Pull) 的详细指南和代码示例，请参阅：**

**[➡️ API 用户手册 (./docs/API_Guide.md)](./docs/API_Guide.md)**

## 🚀 如何构建 (Getting Started)

### 1. 依赖 (Dependencies)

本项目对不同平台的依赖处理方式不同。

#### Linux (服务器端)
**外部依赖：** 你 **必须** 使用系统的包管理器安装以下库：
* `TBB (Intel Threading Building Blocks)`
* `Zlib`
* `Zstd`

*（内置依赖：`nlohmann::json` 和 `moodycamel` 已被包含在项目中，无需安装。）*

#### Windows (客户端)
**依赖已打包：** **无需额外安装依赖！**
* 所有必需的库 (TBB, Zlib, Zstd) 的头文件、静态库 (`.a`/`.lib`) 和动态库 (`.dll`) **均已包含**在 `thirdparty` 目录中。
* 你只需要 MSYS2 提供的 `MinGW64` 编译器和 `CMake` 即可。

---

### 2. 编译 (Building)

#### Linux (服务器端)

```bash
# 1. 安装外部依赖 (以 Ubuntu/Debian 为例)
sudo apt-get update
sudo apt-get install -y libtbb-dev libzstd-dev zlib1g-dev

# 2. 克隆并编译
git clone [https://github.com/AnnieLZZ/MYMQ.git](https://github.com/AnnieLZZ/MYMQ.git)
cd MYMQ
mkdir build && cd build
cmake ..
make
