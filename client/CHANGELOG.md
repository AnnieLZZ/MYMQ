## 📢 版本更新说明 (Release Notes)

### 🏷️ v1.1.0 (2025-11-23)

本次更新聚焦于消息检索（Message Retrieval）架构的性能优化和稳定性提升。

### ✨ 新功能与改进 (New Features and Improvements)

#### 🚀 I/O 线程与消息处理的隔离 (Decoupling I/O and Message Processing)

为了提升客户端的吞吐量，`v1.1.0` 引入了**共享线程池（SharedThreadPool）**机制，根据分区基于Murmurhash2分片，用于将网络 I/O 流程与后续的消息处理流程隔离。


---

## v2.0.0

- **版本更新**: 更新项目版本至 v2.0.0
- **API 调整**: `pull` 接口变动，详见 `API_Guide.md`
- **性能提升**: 优化 `pull` 接收数据解析过程，同测试用例下速度提升至平均 **135806.9 msg/s** (较 v1.0.0 提升约 8.6%)
- **内部方法调整**:zstd，MessageParser类均新增内部方法，crc方法调整，push pull parase_Record等方法的相关部分调整
- **其他**: 测试用例微调
---


### 🏷️ v2.1.0 (2025-11-25)

#### 🚀 I/O 线程与消息处理的隔离 (Decoupling I/O and Message Processing)

`v2.1.0` 
1 优化了消息解析的路径
2 修复了push函数回调设置无效的bug
3 调整了ack的分级，现在仅分 : ACK_NORESPONCE=0,ACK_PROMISE_INDISK=1 
ACK_NORESPONCE时不予设置回调，即发即忘，ACK_PROMISE_INDISK时保证写入服务器pagecache，允许设置回调，其服务器响应包含三个字段：topic partition offset ，offset指当前消息的全局偏移量

---


### 🏷️ v2.2.0 (2025-11-26)

#### 🚀 I/O 线程与消息处理的隔离 (Decoupling I/O and Message Processing)

`v2.2.0` 
1 增加了api : seek ,允许在不提交偏移量的前提下更新本地的偏移量记录
2 增加了一个测试seek能力的测试用例，微调了main测试用例的输出，对应修改了MYMQ\client\CMakeLists.txt
3 业务逻辑调整： 现在如果pull成功返回了非空Record，会自动更新本地的消费offset记录
                现在如果尝试在MYMQ::ACK_Level::ACK_NORESPONCE的ack等级下尝试在push中设置回调，会返回警告并拒绝push调用并返回MYMQ_Public::ClientErrorCode::INVALID_OPRATION


