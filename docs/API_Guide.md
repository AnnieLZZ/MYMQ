

1.  在你的项目根目录创建一个 `docs` 文件夹。
2.  在 `docs` 文件夹内，创建一个新文件，命名为 `API_Guide.md`。
3.  **把下面的所有代码** 复制并粘贴到 `API_Guide.md` 文件中并保存。

<!-- end list -->

````markdown
# MYMQ API 用户手册

本文档将指导你如何初始化客户端、生产和消费消息。

## 启动前：配置文件
> **重要提示：** 所有配置文件 (例如 `config/business.ini`) 均只在程序启动前读取一次。程序运行中对配置文件的修改不会生效。

---

## 1. 初始化与订阅 (Setup & Subscription)

### 1.1 创建客户端
首先，创建一个 `MYMQ_Client` 实例。

```cpp
// 参数1: 客户端ID (可选, 默认 "Client-1")
// 参数2: Push的ACK等级 (可选, 默认 1, 即 ACK_PROMISE_ACCEPT)
std::string clientid = "my-client";
MYMQ::ACK_Level acklevel = MYMQ::ACK_Level::ACK_PROMISE_ACCEPT;

MYMQ_Client mc(clientid, acklevel);
````

  * **ACK 等级 1 (`ACK_PROMISE_ACCEPT`)** 意味着服务器只确认消息已收到且数据没有损坏。

### 1.2 订阅或创建主题

你可以直接订阅一个主题。如果该主题或消费组不存在，服务器将会自动创建它们。

```cpp
mc.subscribe_topic("testtopic");
```

或者，你也可以选择手动创建主题并指定分区数：

```cpp
// 创建一个4分区的名为 "topic1" 的主题
mc.create_topic("topic1", 4);
```

-----

## 2\. 消费者 (Consumer) 指南

接下来的步骤将分离消费者的职责。

> **核心概念 `TopicPartition`:**
> 所有需要指定分区的操作 (Pull, Commit 等)，都必须使用 `MYMQ_Public::TopicPartition` 对象 (在本文档中简称 'tp对') 来指定目标。

### 2.1 加入消费组 (Join Group)

```cpp
mc.join_group("testgroup");
```

  * 请检查控制台输出 "JoinGroup success" 字样。
  * 加入成功后，你可以调用 `get_assigned_partition()` 方法来获取你被分配到的分区列表。

### 2.2 拉取消息 (Pull)

拉取消息是一个**阻塞**操作，它将等待服务器返回数据。

```cpp
// 1. 定义你要拉取的分区
MYMQ_Public::TopicPartition tp("testtopic", 0);

// 2. 执行拉取 (从全局偏移量 0 开始)
auto res = mc.pull(tp, 0);

// 3. 检查拉取结果
if (res.second == Err_Client::NULL_ERROR) {
    // 拉取成功
    // res.first 是一个 std::queue<MYMQ::MYMQ_Client::ConsumerRecord>
} else {
    // 拉取失败, res.second 包含错误码
}
```

### 2.3 处理消息 (Process Records)

拉取是以批次 (Batch) 为单位的，一次 `pull` 可能返回一个包含多条消息的队列。

```cpp
// (续上一步)
std::queue<MYMQ::MYMQ_Client::ConsumerRecord> msg_queue = res.first;

if (!msg_queue.empty()) {
    // 获取队列中的第一条消息
    auto msg1 = msg_queue.front(); // msg1 是 ConsumerRecord 对象
    
    // 调用方法查看消息详情
    std::string topic = msg1.getTopic();
    size_t partition  = msg1.getPartition();
    size_t offset     = msg1.getOffset();
    std::string key   = msg1.getKey();
    std::string value = msg1.getValue();
    int64_t time      = msg1.getTime();
}
```

### 2.4 同步提交偏移量 (Commit Sync)

> **注意：** 手动提交前，请先确认 `config/business.ini` 文件中 `'autocommit'` 字段被置为 `0` (禁用)。

此方法会**阻塞**主线程，直到提交超时或收到服务器的确认响应。

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
size_t new_offset = 100;

mc.commit_sync(tp, new_offset);
```

### 2.5 异步提交偏移量 (Commit Async)

此方法**不会**阻塞主线程。

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
size_t new_offset = 100;

// 1. 简单异步提交 (不关心结果)
mc.commit_async(tp, new_offset);

// 2. 异步提交 (带回调)
// 假设你有一个回调函数:
// void MyCommitCallback(const MYMQ_Public::CommitAsyncResponce& resp) { ... }

mc.commit_async(tp, new_offset, MyCommitCallback);
```

### 2.6 自动提交 (Auto Commit)

如果你不想麻烦地手动提交，可以启用自动提交：

1.  将 `config/business.ini` 文件中的 `'autocommit'` 字段置为 `1` (启用)。
2.  同时设定 `'autocommit_perior_ms'` 的值（例如 `5000`），它将作为自动提交的间隔时间（毫秒）。

-----

## 3\. 生产者 (Producer) 指南

接下来的步骤将分离生产者的职责。

### 3.1 同步推送 (Sync Push)

此方法会**阻塞**，直到收到服务器（根据 `acklevel` 设定的）确认。Key 和 Value 均可为空。

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
std::string key1 = "key1";
std::string val1 = "val1";

Err_Client err = mc.push(tp, key1, val1);
// 检查 err 变量来确认推送是否成功
```

### 3.2 异步推送 (Push with Callback)

此方法**不会**阻塞。你可以在第5个参数传入一个回调函数（类型为 `MYMQ_Public::SupportedCallbacks`），以便在推送完成时收到通知。

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
std::string key1 = "key1";
std::string val1 = "val1";

// 假设你有一个回调函数:
// void MyPushCallback(const MYMQ_Public::PushResponce& resp) { ... }

mc.push(tp, key1, val1, MyPushCallback);
```

> **注意：** 当 `acklevel` 设为 `0` (`MYMQ::ACK_Level::NORESPONCE`) 时，服务器不会返回任何响应，因此设置的回调将**永远不会**被触发。

