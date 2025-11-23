# MYMQ: High-Performance Distributed Message Queue

> A C++ distributed messaging system benchmarked against Apache Kafka's architecture.
> **Role:** Core Developer | **Lang:** C++17

---


## ⚡ 核心性能 (Performance Benchmark)

单机单分区 (Single Node, Single Partition) 环境，消息体大小 200~300B：

| Metric | Throughput |
| :--- | :--- |
| **Push (Producer)** | **> 133,000 msg/s** |
| **Poll (Consumer)** | **> 109,000 msg/s** |

## 🚀 架构设计 (Architecture Features)

### 1. I/O 与存储优化 (I/O & Storage)
* **Zero-Copy with kTLS:** 结合 Linux `sendfile` 与 `mmap` 减少内核态/用户态拷贝；引入 **OpenSSL kTLS (Kernel TLS)** 将加密操作卸载至内核，在保障传输安全的同时维持 sendfile 的零拷贝特性。
* **Page Cache 顺序写优化:** 利用 Linux Page Cache 特性优化写入性能。实际测试中，使用 `write` 和 `writev` 系统调用（大部分场景为 write）替代内存映射进行持久化，在 8KB~32KB 大文件的顺序写场景下，速率相比 mmap 提升 **12倍**。
* **Log-Structured Storage:** 采用“日志段 (Log Segment) + 稀疏索引”结构，实现 $O(\log n)$ 级寻址。
* **Compression:** 消息采用紧凑二进制排布，支持 **Batch 聚合** 与 **ZSTD** 压缩，利用 Page Cache 读写优势并提高带宽利用率。

### 2. 并发模型 (Concurrency Model)
* **FD-Sharded Thread Pool:** 引入基于客户端FD的**分片式线程池**，底层任务队列采用 `moodycamel::BlockingConcurrentQueue`。相比于自旋锁或非阻塞队列，该设计有效避免了线程空闲时的 CPU 空转，降低了系统资源消耗。结合连接哈希分片策略，显著减少了线程间的上下文切换与锁竞争，提升多连接场景下的处理效率。
* **Session-Based Decoupling:** 封装 `TcpSession` 类作为网络层与业务层的交互桥梁，实现**状态机与业务逻辑的解耦**：
    * **封装性:** 业务层无需感知底层通信状态机 (FSM) 细节，仅需通过 Session 对象即可安全地发送响应。
    * **生命周期管理:** Session 内部持有状态机的 `shared_ptr`。业务层通过拷贝 Session 对象即可在任意上下文（包括异步延时任务、长耗时任务）中安全回调。
    * **无锁化优化:** 避免长时业务任务长时间占用 TBB Map 的桶元素，消除了对其他线程查找连接映射的性能干扰，保证了高并发下核心索引的高效访问。
* **Lock-Free Queue:** 通信层使用 `moodycamel::ReaderWriterQueue` (**SPSC 无锁队列**) 减少线程竞争和锁开销。
* **Event-Driven:** 基于 `epoll` + `Reactor` + `SSL通信` 模式，配合有限状态机 (FSM) 处理并发连接与事务。

### 3. 分布式协同 (Distributed Coordination)
* **Incremental Cooperative Rebalancing:** 实现了 Kafka 版本的“增量协作式重平衡”机制，相比传统停顿方式，提高了不稳定消费者组的协作效率。
* **Group Coordinator:** 内置组协调器协议，自动管理分区分配、消费者心跳及 Offset 提交。

### 4. 安全与可靠性 (Security & Reliability)
* **Data Integrity:** 全链路以及 **RecordBatch** 本体均内嵌 `CRC32` 校验，实现端到端的消息完整性保障（覆盖传输和存储过程）。
* **SSL/TLS:** 采用 SSL 协议，基于 DHE-RSA-AES128-SHA256 等安全套件，通过双向认证和加密通信确数据的机密性和身份可信。

## 🛠️ 技术栈 (Tech Stack)

* **Kernel/Network:** `Epoll`, `Reactor`, `Linux sendfile`, `OpenSSL kTLS`
* **Concurrency:** `Intel TBB`, `FD-Sharding Pool`, `moodycamel::BlockingConcurrentQueue`, `moodycamel::ReaderWriterQueue (Lock-Free)`, `C++14 Threads`
* **Storage/Algo:** `write/writev (Sequential Write)`, `mmap (Read/Zero-Copy)`, `ZSTD`, `Sparse Indexing`, `CRC32`
* **Build/Test:** `CMake`, `GTest`

---

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
---
### 🚀 运行 Windows 客户端 (重要！)

Windows 客户端依赖动态库 (例如 `tbb.dll`)。

在 `cmake --build .` 编译完成后，你会在 `client/build/` 目录（或 `client/build/src` 之类的地方）找到生成的 `.exe` 可执行文件。

* **直接运行 `.exe` 会失败**，因为它找不到所需的 `.dll` 文件。

**解决方法:**

你需要将 `client/thirdparty` 目录中用到的 **`.dll` 文件**（例如 `client/thirdparty/tbb/bin/tbb.dll` 等）**复制到 `.exe` 文件所在的同一目录下**，然后再运行。
