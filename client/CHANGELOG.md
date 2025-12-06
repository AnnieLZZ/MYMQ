## 📢 版本更新说明 (Release Notes)

### 🏷️ v1.1.0 (2025-11-23)

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


`v2.1.0` 
1 优化了消息解析的路径
2 修复了push函数回调设置无效的bug
3 调整了ack的分级，现在仅分 : ACK_NORESPONCE=0,ACK_PROMISE_INDISK=1 
ACK_NORESPONCE时不予设置回调，即发即忘，ACK_PROMISE_INDISK时保证写入服务器pagecache，允许设置回调，其服务器响应包含三个字段：topic partition offset ，offset指当前消息的全局偏移量


---
### 🏷️ v2.2.0 (2025-11-26)

`v2.2.0` 
1 增加了api : seek ,允许在不提交偏移量的前提下更新本地的偏移量记录
2 增加了一个测试seek能力的测试用例，微调了main测试用例的输出，对应修改了MYMQ\client\CMakeLists.txt
3 业务逻辑调整： 现在如果pull成功返回了非空Record，会自动更新本地的消费offset记录
                现在如果尝试在MYMQ::ACK_Level::ACK_NORESPONCE的ack等级下尝试在push中设置回调，会返回警告并拒绝push调用并返回MYMQ_Public::ClientErrorCode::INVALID_OPRATION
---

## v2.4.0(2025-11-27)

- **版本更新**: 更新项目版本至 v2.4.0
- **性能提升**:  现在epoll_ctl:recvmsg调用比降低至37：36
                网络IO缓冲区大小改为配置项,默认16384（16k，==ssl_read_max）
                优化了一次包体移动开销
                优化后台线程和定时器架构，在测试用例4,000,000 msg 200-300B
                 用例下速度提升至平均 **244806.9 msg/s** (较 v2.0.0 提升约 180.7%)
- **其他**: 测试用例调整


---

## v3.0.0(2025-12-5)

- **版本更新**: 更新项目版本至 v3.0.0
- **更新细则**: 
 1 取消了定时触发poll的逻辑，当前来说这个逻辑没有意义
2 增大了config的最大飞行请求数，防止高频请求时队列快速扩张导致拒绝请求
3 将测试用例pull的延迟抬高，收集更多chuck并发解析
4 request_timeout_s改为系统配置项request_timeout_ms，默认为为5000
5 新增'Request_timeout_queue.h' 用deque加静默删除的模式，用于高并发场景下检测超时的timer，代替原有timer，平均每次行为从o(logn)降至o(1)，同时去掉了线程池，改为拒绝无意义线程压力和投递任务压力
6pull第二个参数改为poll_wait_timeout_ms，用于精确调控等待时间
- **其他**: 测试用例调整

---

## v3.1.0(2025-12-6)

- **版本更新**: 更新项目版本至 v3.1.0
- **更新细则**: 
1 删除autopoll_perior_ms，pull_bytes更名pull_bytes_once_of_request
2 测试用例不再是随机字符，而是按可以按模式修改的常见模拟格式字符串
3 local_pull_bytes_once 改为运行时配置项，新增’set_local_pull_bytes_once‘ api来设置该值
4 移除自动拉取相关逻辑
5 新增用于开发测试的pull重载版本，传入最后一个参数int64_t来获取pull实际运行时间，单位us
6 修正心跳逻辑，当世代不符时就代表需要重入组
- **其他**: 测试用例调整


---

## v3.2.0(2025-12-6)

- **版本更新**: 更新项目版本至 v3.2.0
- **更新细则**: 
1 新增了MYMQ_Produceruse类，外部用MYMQ_Producer，调用原类的生产者方法
2 新增了MYMQ::MYMQ_Client::RecordAccumulator，是对map_pushqueue的封装
- **其他**: 测试用例调整


