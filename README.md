# MYMQ

> (Learning Project) A high-performance C++ MQ built from scratch, inspired by Kafka.
> 
> (学习项目) 一个从零构建的、受 Kafka 启发的 C++ 高性能消息队列。

## 🌟 核心特性 (Features)

* **极致的 I/O 效率 (Extreme I/O Efficiency)**
    * 利用 Linux `sendfile` 和内存映射 (`mmap`) 避免不必要的内核态/用户态拷贝。
    * 消息以二进制密排布，并使用 `ZSTD` 高度压缩 + 聚合 Batch 结构存储，大幅降低磁盘和网络开销。

* **高性能网络模型 (High-Perf Network Model)**
    * 服务器端采用 `epoll` + `Reactor` + 状态机（FSM）管理多链接非阻塞 I/O。
    * 自定义线程池和定时器高效处理心跳、请求重试等长时任务。

* **开创性重平衡 (Incremental Rebalancing)**
    * 采用增量协作式的重平衡策略，高效解决传统消费者断线重连时的“Stop-The-World”重平衡痛点。

* **Kafka 核心设计 (Kafka's Core Design)**
    * 在单节点上实现了 Kafka 的核心机制：日志段 + 索引（高效磁盘读写）、组协调器（Group Coordinator）、分区分配策略等。

* **工业级并发组件 (Industry-Grade Concurrency)**
    * 使用 `moodycamel::ReaderWriterQueue` (SPSC无锁队列) 作为高性能通信缓冲。
    * 使用 `TBB::concurrent_hash_map` 处理高并发下的消费偏移量读写。

* **可靠性与跨平台 (Reliability & Cross-Platform)**
    * 消息完整性由 `CRC32` 校验保证。
    * 服务器端 (Linux) 和 客户端 (Windows) 分别优化。

## 🛠️ 技术栈 (Technology Stack)

* **网络:** `epoll` + `Reactor`
* **并发:** `Intel TBB`, `moodycamel::ReaderWriterQueue`
* **I/O:** `sendfile`, `mmap`
* **压缩:** `zstd`
* **校验:** `zlib::crc32`

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

Windows (客户端)
重要提示: 请确保你运行的是 MSYS2 MinGW 64-bit 终端 (mingw64.exe)。
Bash

# 1. (如果还未安装) 确保 CMake 和 MinGW 工具链已安装
pacman -S --needed base-devel mingw-w64-x86_64-toolchain mingw-w66-x86_64-cmake# 2. 克隆并编译 (CMake 会自动查找 thirdparty 目录下的库)
git clone [https://github.com/AnnieLZZ/MYMQ.git](https://github.com/AnnieLZZ/MYMQ.git)cd MYMQ
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
3. 运行 Windows 客户端 (重要！)
Windows 客户端依赖动态库 (如 tbb.dll)。在 cmake --build . 编译完成后，你会在 build 目录（或 build/src/client 之类的地方）找到生成的 .exe 可执行文件。
直接运行 .exe 会失败，因为它找不到 ..dll 文件。
解决方法：
你需要将 thirdparty 目录中用到的 .dll 文件（例如 thirdparty/tbb/bin/tbb.dll 等）复制到 .exe 文件所在的同一目录下，然后再运行。










