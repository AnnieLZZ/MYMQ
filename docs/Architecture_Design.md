# MYMQ 架构文档（代码事实版）


## 1. 项目边界与目录

- `server/`：服务端可执行程序，入口在 `server/main.cpp`。
- `client/`：客户端库（`mymq_lib`）和示例程序，入口 API 在 `client/include/MYMQ_C.h`。
- `docs/`：说明文档（非运行时代码）。

构建方面：

- 客户端 `client/CMakeLists.txt` 构建静态库 `mymq_lib`，并可构建示例。
- 服务端 `server/CMakeLists.txt` 构建库 `MYMQ` 和可执行 `MyMQ_App`。

## 2. 对外 API（客户端）

`client/include/MYMQ_C.h` 暴露两个类：

- `MYMQ_Producer`
  - `push(tp,key,value,cb)`
  - `create_topic(topic, parti_num)`
  - `stop()`
- `MYMQ_Consumer`
  - `subscribe_topic / unsubscribe_topic`
  - `join_group / leave_group`
  - `pull(...)`（两个重载）
  - `commit_sync / commit_async`
  - `seek`
  - `get_assigned_partition / get_is_ingroup`
  - `set_pull_max_record_num_local / set_pull_fetch_min_bytes`
  - `trigger_pull`

这两个类是 PImpl 包装，实际逻辑在 `client/src/MYMQ_Client.cpp` 的 `MYMQ_Produceruse` 和 `MYMQ_Consumeruse`。

## 3. 线协议与错误码

协议头固定 12 字节（定义在 `MYMQ::HEADER_SIZE=12`）：

- `uint32 total_length`
- `uint16 event_type`
- `uint32 correlation_id`
- `uint16 ack_level`

协议事件枚举在 `client/src/MYMQ_innercodes.h` 的 `EventType`：

- 请求：`CLIENT_REQUEST_PUSH/PULL/COMMIT_OFFSET/HEARTBEAT/REGISTER/CREATE_TOPIC/...`
- 响应：`SERVER_RESPONSE_PUSH_ACK/PULL_DATA/HEARTBEAT/COMMIT_OFFSET/...`

公共错误码与客户端错误码在 `client/include/MYMQ_PublicCodes.h`：

- `CommonErrorCode`
- `ClientErrorCode`
- `ChannelRole`（`UNKNOWN/CONTROL/FETCH/PRODUCE`）

## 4. 服务端架构（代码结构）

## 4.1 启动链路

- `server/main.cpp` 构造 `MYMQ_S`
- `server/src/MYMQ_S.cpp` 中 `MYMQ_S` 构造 `MessageQueue`
- `MessageQueue` 构造函数中完成：
  - `init()` 读取配置并初始化线程池和 `MetadataCache`
  - `load_topics_metadata()`
  - 创建 `ConsumerOffset`
  - `start_groupcoordinator()`
  - `start_server()`

## 4.2 网络与事件循环

`server/src/Server.h` 中的 `Server`：

- 使用 `epoll` + 非阻塞 socket。
- 使用 OpenSSL，握手成功后可检测 kTLS 发送能力。
- 每连接状态在 `ClientState`：
  - 读状态机：读 header -> 读 body
  - 发送队列：普通包或 `FileSendTask`
- 连接事件分发到 `ShardedThreadPool::instance().submit(fd, ...)`，按 fd 分片。

## 4.3 请求分发（业务入口）

`MessageQueue::start_server()` 内注册回调并按 `event_type` 分支处理：

- `CLIENT_REQUEST_PULL`
- `CLIENT_REQUEST_PUSH`
- `CLIENT_REQUEST_COMMIT_OFFSET`
- `CLIENT_REQUEST_REGISTER`
- `CLIENT_REQUEST_LEAVE_GROUP`
- `CLIENT_REQUEST_HEARTBEAT`
- `CLIENT_REQUEST_CREATE_TOPIC`

该分发同时检查通道角色（`CONTROL/FETCH/PRODUCE`）与请求匹配关系。

## 4.4 长轮询（Pull）

在 `MessageQueue` 中：

- 拉取失败 `NO_RECORD` 时，请求进入 `pending_fetches_`（按 `TopicPartition` 组织）。
- 新写入成功时 `try_complete_purgatory(...)` 尝试补发。
- 定时器周期执行 `check_purgatory_expiration()`，超时返回错误响应。
- 分区撤销时 `on_partition_revocation(...)` 强制返回 `REBALANCE_IN_PROGRESS`。

## 5. 存储架构（代码结构）

核心类在 `server/src/MessageQueue.h` 和 `server/src/Logsegment.h`：

- `Partition`
- `PartitionStorage`
- `LogSegment`

实现要点（代码可见）：

- 每个分区落盘在 `data_root/topic/ParX`。
- 分段文件由 `.log` + `.index` 组成。
- `PartitionStorage::save_msg`：
  - 先共享锁写当前段
  - 满段时升级独占锁并轮转新段（双重检查）
- `LogSegment::recover_index`：
  - 启动时扫描日志重建索引
  - 发现坏尾时截断到最后有效位置
- 定时任务：
  - 周期 flush（日志/索引）
  - 可触发 compact（代码中用于旧段整理）

## 6. 消费组与提交语义

状态相关代码在 `server/include/MYMQ_Server_ns.h`：

- `ConsumerGroupState`
- `ServerConsumerInfo`
- `GroupCoordinator`

关键状态：

- `generation_id`
- `assigned_partitions`
- `current_holding`
- `target_assignment`
- 分区反向索引 `partition_owners_`

关键行为：

- `update_subscription` 触发 rebalance。
- `heartbeat` 按 generation 协调分配更新。
- `commit_offset` 需要同时满足：
  - member 存在
  - generation 匹配
  - member 当前持有该分区
- commit 同时会写入 `__consumer_offset` 主题分区（在 `MessageQueue` 的 commit 分支可见）。

## 7. 客户端架构（代码结构）

## 7.1 通信对象

`client/src/Communication.h` 的 `Communication_client`：

- 管理 TLS 连接和 IO 线程。
- 维护发送队列、待响应回调映射和超时队列。
- 通过 `Correlation_ID` 跟踪请求/响应。
- 支持通道角色设置与注册（CONTROL/FETCH/PRODUCE）。

## 7.2 Producer 路径

`MYMQ_Produceruse` 关键流程：

1. `push()` 写入分区对应 `Push_queue` 的 `active_buf`。
2. `active_buf` 满后进入 `ready_queue`。
3. 按 `topic+partition` hash 提交到 `ShardedThreadPool` 执行 `flush_batch_task`。
4. `flush_batch_task` 打包并发送；响应后按稀疏回调列表回放到消息级回调。
5. 背压条件：`ready_queue.size() >= push_max_queued_batches` 时等待或返回队列满错误。

## 7.3 Consumer 路径

`MYMQ_Consumeruse` 关键流程：

1. `join_group()` 通过 heartbeat 入组。
2. 收到分配后构造本地 `map_final_assign`。
3. `trigger_poll_for_low_cap_pollbuffer()` 对需要拉取的分区发 pull 请求。
4. pull 响应进入 `PollBuffer`，并通知 `cv_poll_ready`。
5. `pull()` 收集多个分区数据后用 `tbb::parallel_for_each` 解析。
6. `commit_sync/commit_async` 提交消费位点。

## 8. 配置与运行时行为

代码中直接读取的配置文件：

- 客户端：
  - `client/config/communication.ini`
  - `client/config/sys.ini`
  - `client/config/business.ini`
- 服务端：
  - `server/config/communication.propertity`
  - `server/config/system.properity`
  - `server/config/storage.properity`

这些配置直接影响：

- 线程数、in-flight 限额、请求超时
- 批大小、自动推送周期、自动提交周期
- 拉取最小字节与本地缓冲阈值
- 段刷新/清理周期

## 9. 当前代码已体现的优化手段

- 连接分片并发：服务端按 fd 分片调度。
- 角色分离通道：客户端 control/fetch/produce 分开连接。
- 消息批处理：producer 端缓冲并批量发送。
- 稀疏回调：仅保存需要回调的消息索引。
- 长轮询：服务端挂起 pull，写入后主动补发。
- 分区反向索引：消费组判断 owner 时避免全量遍历。
- 启动恢复：日志坏尾可截断，减少无法启动风险。

## 10. 代码可见的改进方向（不改变现状陈述）

- 增加指标与 tracing：当前主要靠日志，缺少标准化运行指标。
- 配置校验与单位统一：当前字节/毫秒配置较多，易误配。
- README 与 API 文档同步自动化：减少文档与实现偏离。
- 将协议字段变更流程显式化：减少客户端/服务端版本错配风险。

---

本文件更新原则：后续如需修改，请以源码为准，先改代码后改文档。
