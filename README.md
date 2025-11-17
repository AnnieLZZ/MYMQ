# MYMQ

> (Learning Project) A high-performance C++ MQ built from scratch, inspired by Kafka.
> 
> (学习项目) 一个从零构建的、受 Kafka 启发的 C++ 高性能消息队列。

## 🌟 核心特性 (Features)

* **极高性能 (Blazing Fast):** 专为 C++ 优化，单分区 Push 吞吐量可达 100,000 msg/s，Pull 吞吐量 90,000 msg/s (消息平均长度 200-300 字节)。
* **单节点架构 (Single-Node):** 实现了 Kafka 的核心概念（如分区、持久化日志），但摒弃了分布式的复杂性，实现了极低的延迟。
* **现代 C++ 技术栈 (Modern C++ Stack):** * 使用 `Intel TBB` 进行并发控制。
    * 使用 `moodycamel::ConcurrentQueue` (无锁队列) 处理内部数据流。
    * 使用 `zstd` 进行高效的消息压缩。

## 🚀 如何构建 (Getting Started)

本项目在 Linux 上开发和测试。

### 1. 依赖 (Prerequisites)

在编译前，请确保你已安装以下库：

* TBB (Intel Threading Building Blocks)
* Zlib
* Zstd

*(这里你可以补充你的编译命令, 比如 `cmake`)*
