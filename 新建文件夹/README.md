# MYMQ: High-Performance Distributed Message Queue

> A C++ distributed messaging system benchmarked against Apache Kafka's architecture.
> **Role:** Core Developer | **Lang:** C++17


---

## ⚡ 核心性能 (Performance Benchmark)

### 📊 单机单分区性能指标 (Single Node, Single Partition)

**测试环境:** 
* Workload: 4,000,000 msgs | Size: 200~300B | 单分区 (Single Partition)
* Hardware: C端: [CPU: Intel® Core™ i7-12650H (10 Cores)] | [Disk: NVMe SSD]
            S端: [CPU: Intel® Core™ i7-12650H ] | [Disk: NVMe SSD] | [处理器:2 / 内核数: 4]

| Metric | Throughput | Description |
| :--- | :--- | :--- |
| **Push (Producer)** | **~331,198 msg/s** | **End-to-End**: User API $\rightarrow$ Server PageCache $\rightarrow$ ACK $\rightarrow$ Client Callback Execution |
| **Poll + commitsync (Consumer)** | **~255,983 msg/s** | **Fetch & Parse & commit**: Client Response Handling + Message Deserialization +commitsync |

---

## 🚀 架构设计 (Architecture Features) Version4.0.0

### 1. I/O 与存储优化 (I/O & Storage)
* **Zero-Copy with kTLS:** 结合 `sendfile` 实现零拷贝传输；引入 **OpenSSL kTLS** 将加密卸载至内核态，解决了传统 SSL 在用户态加密导致无法利用 sendfile 的痛点，显著减少内核/用户态上下文切换。（这也是为什么不使用boost.asio的原因，boost.asio强制将加密抬到用户态）
* **混合存储策略:**
    * **日志段:** 采用标准 `write` 系统调用进行 Append-only 追加写。利用 Linux Page Cache 的顺序写合并机制，避免了 `mmap` 在处理变长文件追加时频繁触发的缺页中断和 TLB 刷新。
    * **稀疏索引:** 采用 `mmap` 内存映射。针对固定小步长递增的索引文件，利用内存映射避免读取时的 buffer 拷贝，以 $O(\log n)$ 效率的二分查找来消息辅助定位。
* **Log-Structured:** 采用标准“分段日志 + 稀疏索引”结构。**基于 Base Offset 命名日志段及其索引**，支持**按段大小自动滚动**，保证了磁盘空间的有序管理与写入性能的线性扩展。
* **原生批量架构:**
    * **强制聚合:** 摒弃单条消息传输，**强制**采用 RecordBatch 形式进行全链路传输与存储。
    * **设计考量:**
        1. **I/O 吞吐:** 配合 Linux Page Cache 机制，大块数据的顺序 `write` 能极大提升内核写缓冲效率与磁盘带宽利用率。
        2. **ZSTD 压缩收益:** 大块数据提供了更丰富的上下文，显著提升 **ZSTD** 的字典匹配效率与压缩比，克服了小包压缩率低的缺陷。
        3. **网络效率:** 均摊了系统调用开销，显著减少网络往返与 TCP 包头开销。
        4. Record采用紧凑二进制布局，最大化 CPU 缓存命中率。

### 2. 并发设计 (Concurrency Design)
**分片式线程池** 底层使用 `moodycamel::BlockingConcurrentQueue`。该设计保证了同一连接的请求处理具备 CPU 亲和性，大幅减少线程间的上下文切换与CPU空转。 
#### 服务端 (Broker Side)
* **FD-Sharded Thread Pool:** 引入基于连接 FD 哈希的分片式线程池，实现线程间负载均衡的同时，确立了连接维度的 CPU 亲和性，有效避免了多线程处理同一连接上下文时的锁开销与缓存失效。
#### 架构
* **Event-Driven:** 基于 `epoll` (ET模式) + `Reactor` 模式，配合非阻塞 I/O 与有限状态机 (FSM) 处理高并发连接。

#### 客户端 (Client Side)
* **Partition-Aware Response Sharding:** 针对 Consumer 的消息拉取（Pull）响应 和 Producer的推送（Push），设计了专用的分片线程池。
    * **路由策略:** 基于 `Topic + Partition` 组合键用 **MurmurHash2** 计算出key来进行**分片**，均摊线程压力，同时保证同一分区的数据流固定路由至同一工作线程。
    * **收益:** 实现了消息解析与业务处理的并行化，保证单分区内消息处理的时序性同时显著提升了高吞吐场景下的生产和消费速率。
* **双缓冲队列:**  针对Producer生产速率过快，利用双缓冲队列+分片线程池进行背压和并行优化
#### 组件
* **Lock-Free Queue:** 通信层内部使用 `moodycamel::ReaderWriterQueue` (**SPSC**)作发送队列 ，以及作将拉取到的消息连接到用户应用程序的通道。

### 3. 异步与解耦 
#### 服务端 (Broker Side)
* **Session-Based Decoupling:** 封装 `TcpSession` 实现网络层 (Reactor) 与业务层的解耦：
    * 利用 `shared_ptr` 延长 Session 生命周期，确保在异步/长耗时任务回调中对象的安全性。
    * 业务层通过持有 Session 副本发送响应，无需长时间占用状态机映射 (TBB Map) 的锁资源，保障了高并发下核心索引的访问效率。

#### 客户端 (Client Side)
* **Granular Async Callbacks:** 提供全异步的事件驱动接口。
    * **Per-Message Callback:** 支持在 `push` 阶段为**每一条**消息单独注册回调函数，而非仅针对 Batch 级别。
    * **Commit Callback:** `commitAsync` 支持异步回调通知。
    * **Execution Flow:** 回调函数在客户端接收到服务端 ACK 并完成解析后自动触发，实现了从发送到确认的全链路闭环。


### 4. 分布式协同 (Distributed Coordination)
* **Incremental Cooperative Rebalancing:** 实现了 Kafka 协议的“增量协作式重平衡”。相比传统的 Eager Rebalancing，该允许消费者在重平衡期间保留部分分区所有权，消除了“Stop-the-world”带来的消费停顿。
* **Group Coordinator:** 内置组协调器，管理消费者组状态、分区分配策略、心跳检测及 Offset 提交。

### 5. 安全与可靠性 (Security & Reliability)
* **Data Integrity:** 实现了端到端的 **CRC32** 校验（覆盖 RecordBatch 生成、传输、落盘全链路），防止网络翻转或磁盘静默错误导致的数据损坏。
* **SSL/TLS:** 支持双向认证，基于 DHE-RSA-AES128-SHA256 等安全套件保障通信机密性。

---

## 🛠️ 技术栈 (Tech Stack)

* **Kernel/Network:** `Epoll (ET)`, `Reactor Pattern`, `Linux sendfile`, `OpenSSL kTLS`
* **Concurrency:** `Intel TBB`, `Sharded ThreadPool (FD & Partition)`, `MurmurHash2`, `moodycamel`, `C++17`
* **Storage/Algo:** `write (Sequential Log)`, `mmap (Index)`, `ZSTD`, `Sparse Indexing`, `CRC32`
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

*（内置依赖： `moodycamel` 已被包含在项目中，无需安装。）*

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
另外注意自己linux的版本，然后下载openssl3.x系列

# 2. 克隆仓库
git clone [https://github.com/AnnieLZZ/MYMQ.git](https://github.com/AnnieLZZ/MYMQ.git)
cd MYMQ

# 3. (重要) 进入服务器目录
cd server

# 4. 编译
mkdir build && cd build
cmake ..
make

# 5. 服务器数字证书
可选择默认配置:
openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout server.key -out server.crt

生成后将 `server.key` 和 `server.crt` 文件移动到项目的build目录内

---
#### Windows (客户端)

MYMQ Windows 客户端编译与运行指南 (MSYS2 MinGW 64-bit)

> **重要提示:** 请确保你运行的是 **MSYS2 MinGW 64-bit 终端** (`mingw64.exe`)。

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
