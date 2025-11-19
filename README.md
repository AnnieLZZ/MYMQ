# MYMQ: High-Performance Distributed Message Queue

> A C++ distributed messaging system benchmarked against Apache Kafka's architecture.
> **Role:** Core Developer | **Lang:** C++17

## ⚡ 核心性能 (Performance Benchmark)

在单机单分区 (Single Node, Single Partition) 环境下，处理 200~300B 消息体：

| Metric | Throughput |
| :--- | :--- |
| **Push (Producer)** | **> 100,000 msg/s** |
| **Poll (Consumer)** | **> 95,000 msg/s** |

## 🚀 架构亮点 (Key Features)

### 1. 极致 I/O 与存储 (Extreme I/O & Storage)
* **Zero-Copy with kTLS:** 深度整合 Linux `sendfile` 与 `mmap` 消除内核态/用户态拷贝；创新性引入 **OpenSSL kTLS (Kernel TLS)**，将加密卸载至内核，在保障传输安全的同时维持零拷贝特性。
* **Log-Structured Storage:** 采用“日志段 (Log Segment) + 稀疏索引”结构，结合 Linux Page Cache 实现极速顺序写与 O(1) 级消息寻址。
* **High Compression:** 消息采用紧凑二进制排布，支持 **Batch 聚合** 与 **ZSTD** 压缩，最大化磁盘与带宽利用率。

### 2. 工业级并发模型 (Industry-Grade Concurrency)
* **Lock-Free Architecture:** 通信层采用 `moodycamel::ReaderWriterQueue` (**SPSC 无锁队列**) 彻底消除线程竞争与锁开销。
* **Concurrent Structures:** 核心索引与元数据管理集成 `Intel TBB` (`concurrent_hash_map`)，确保高并发下的线程安全与访问效率。
* **Event-Driven Core:** 基于 `epoll` + `Reactor` 模式，配合 **有限状态机 (FSM)** 处理海量非阻塞连接与长时任务。

### 3. 分布式与高可用 (Distributed System)
* **Incremental Cooperative Rebalancing:** 实现了 Kafka 现代版的“增量协作式重平衡”，摒弃传统的 Stop-The-World 机制，确保消费者组在变更时业务不中断。
* **Group Coordinator:** 内置组协调器协议，自动化管理分区分配、消费者心跳及 Offset 提交。
* **Data Integrity:** 全链路集成 `CRC32` 校验，保障数据从写入到消费的绝对完整性。

## 🛠️ 技术栈 (Tech Stack)

* **Kernel/Network:** `Epoll`, `Reactor`, `Linux sendfile`, `OpenSSL kTLS`
* **Concurrency:** `Intel TBB`, `moodycamel::ReaderWriterQueue (Lock-Free)`, `C++11 Threads`
* **Storage/Algo:** `Memory Mapped File (mmap)`, `ZSTD`, `Sparse Indexing`, `CRC32`
* **Build/Test:** `CMake`, `GTest`



## 📖 如何使用 (How to Use)

**关于如何调用 API (例如 Push/Pull) 的详细指南和代码示例，请参阅：**

**[➡️ API 用户手册 (./docs/API_Guide.md)](./docs/API_Guide.md)**

## 🚀 如何构建 (Getting Started)

本项目包含 `client/` 和 `server/` 两个独立的子项目。

### 1. 依赖 (Dependencies)

#### Linux (服务器端)
**外部依赖：** 你 **必须** 使用系统的包管理器安装以下库：
* `TBB (Intel Threading Building Blocks)`
* `Zlib`
* `Zstd`

*（内置依赖：`nlohmann::json` 和 `moodycamel` 已被包含在项目中，无需安装。）*

#### Windows (客户端)
**依赖已打包：** **无需额外安装依赖！**
* 所有必需的库 (TBB, Zlib, Zstd) 的头文件、静态库 (`.a`/`.lib`) 和动态库 (`.dll`) **均已包含**在 `client/thirdparty` 目录中。
* 你只需要 MSYS2 提供的 `MinGW64` 编译器和 `CMake` 即可。

---

### 2. 编译 (Building)

#### Linux (服务器端)

```bash
# 1. 安装外部依赖 (以 Ubuntu/Debian 为例)
sudo apt-get update
sudo apt-get install -y libtbb-dev libzstd-dev zlib1g-dev

# 2. 克隆仓库
git clone [https://github.com/AnnieLZZ/MYMQ.git](https://github.com/AnnieLZZ/MYMQ.git)
cd MYMQ

# 3. (重要) 进入服务器目录
cd server

# 4. 编译
mkdir build && cd build
cmake ..
make

MYMQ Windows 客户端编译与运行指南 (MSYS2 MinGW 64-bit)

> **重要提示:** 请确保你运行的是 **MSYS2 MinGW 64-bit 终端** (`mingw64.exe`)。

### 💻 Bash 编译步骤

1.  **安装依赖 (如果还未安装)**
    确保 `CMake` 和 `MinGW` 工具链已安装。

    ```bash
    pacman -S --needed base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake
    ```

2.  **克隆仓库**

    ```bash
    git clone [https://github.com/AnnieLZZ/MYMQ.git](https://github.com/AnnieLZZ/MYMQ.git)
    cd MYMQ
    ```

3.  **(重要) 进入客户端目录**

    ```bash
    cd client
    ```

4.  **编译**
    `CMake` 会自动查找 `../thirdparty` 目录下的库。

    ```bash
    mkdir build && cd build
    cmake -G "MinGW Makefiles" ..
    cmake --build .
    ```

### 🚀 运行 Windows 客户端 (重要！)

Windows 客户端依赖动态库 (例如 `tbb.dll`)。

在 `cmake --build .` 编译完成后，你会在 `client/build/` 目录（或 `client/build/src` 之类的地方）找到生成的 `.exe` 可执行文件。

* **直接运行 `.exe` 会失败**，因为它找不到所需的 `.dll` 文件。

**解决方法:**

你需要将 `client/thirdparty` 目录中用到的 **`.dll` 文件**（例如 `client/thirdparty/tbb/bin/tbb.dll` 等）**复制到 `.exe` 文件所在的同一目录下**，然后再运行。
