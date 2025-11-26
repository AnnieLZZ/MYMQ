## 📢 版本更新说明 (Release Notes)

### 🏷️ v1.1.0 (2025-11-23)

本次更新聚焦于消息检索（Message Retrieval）架构的性能优化和稳定性提升。

### ✨ 新功能与改进 (New Features and Improvements)

#### 🚀 I/O 线程与消息处理的隔离 (Decoupling I/O and Message Processing)

为了提升客户端的吞吐量，`v1.1.0` 引入了**共享线程池（SharedThreadPool）**机制，根据分区基于Murmurhash2分片，用于将网络 I/O 流程与后续的消息处理流程隔离。

---

### 🏷️ v2.1.0 (2025-11-26)

#### 🚀 I/O 线程与消息处理的隔离 (Decoupling I/O and Message Processing)

`v2.1.0` 
1 优化了消息解析的路径
2 修复了push函数回调设置无效的bug
3 调整了ack的分级，现在仅分 : ACK_NORESPONCE=0,ACK_PROMISE_INDISK=1 
ACK_NORESPONCE时不予设置回调，即发即忘，ACK_PROMISE_INDISK时保证写入服务器pagecache，允许设置回调，其服务器响应包含三个字段：topic partition offset ，offset指当前消息的全局偏移量

