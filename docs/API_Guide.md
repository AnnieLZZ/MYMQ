# MYMQ API 用户手册（当前代码版）

本文档基于当前公开接口 `client/include/MYMQ_C.h` 编写。

## 启动前说明

- 配置文件仅在启动时读取一次，运行中修改不会生效。
- 客户端常用配置：
  - `client/config/communication.ini`
  - `client/config/sys.ini`
  - `client/config/business.ini`

## 1. 头文件与基础类型

```cpp
#include "MYMQ_C.h"
#include "MYMQ_PublicCodes.h"
```

常用类型：

- `MYMQ_Producer`
- `MYMQ_Consumer`
- `MYMQ_Public::TopicPartition`
- `MYMQ_Public::ClientErrorCode`
- `MYMQ_Public::PushResponce`
- `MYMQ_Public::CommitAsyncResponce`

## 2. Producer 实际用法

## 2.1 创建 Producer

```cpp
// ack_level:
// 1 = 需要服务端ACK（默认行为）
// 0 = 不等待ACK（回调不会触发）
MYMQ_Producer producer("producer-1", 1);
```

## 2.2 创建主题

```cpp
producer.create_topic("topic_demo", 4);
```

## 2.3 发送消息（无回调）

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
auto err = producer.push(tp, "k1", "v1");

if (err != MYMQ_Public::ClientErrorCode::Success) {
    // 处理错误
}
```

## 2.4 发送消息（带回调）

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);

auto on_push = [](MYMQ_Public::PushResponce resp) {
    // resp.tp / resp.errorcode / resp.offset
};

auto err = producer.push(tp, "k2", "v2", on_push);
```

说明：

- 当 `ack_level=0` 时，服务端不返回 push ack，回调不会触发。
- 发送失败会返回 `ClientErrorCode`，例如 `QUEUE_FULL`、`NOT_REGISTER`、`TIMEOUT`。

## 2.5 停止 Producer

```cpp
producer.stop();
```

## 3. Consumer 实际用法

## 3.1 创建 Consumer

```cpp
MYMQ_Consumer consumer("consumer-1", 1);
```

## 3.2 订阅并入组

```cpp
consumer.subscribe_topic("topic_demo");
consumer.join_group("group_demo");
```

可选检查：

```cpp
bool in_group = consumer.get_is_ingroup();
auto assigned = consumer.get_assigned_partition();
```

## 3.3 主动触发拉取

```cpp
consumer.trigger_pull();
```

## 3.4 拉取消息

```cpp
std::vector<MYMQ_Public::ConsumerRecord> records;
auto err = consumer.pull(records, 5000); // timeout ms

if (err == MYMQ_Public::ClientErrorCode::Success) {
    for (const auto& r : records) {
        auto key = r.getKey();
        auto val = r.getValue();
        auto off = r.getOffset();
    }
}
```

带解析耗时版本：

```cpp
std::vector<MYMQ_Public::ConsumerRecord> records;
int64_t latency_us = 0;
auto err = consumer.pull(records, 5000, latency_us);
```

## 3.5 提交位点

同步提交：

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t next_offset_to_consume = 100;
auto err = consumer.commit_sync(tp, next_offset_to_consume);
```

异步提交（无回调）：

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t next_offset_to_consume = 100;
auto err = consumer.commit_async(tp, next_offset_to_consume);
```

异步提交（带回调）：

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t next_offset_to_consume = 100;

auto on_commit = [](MYMQ_Public::CommitAsyncResponce resp) {
    // resp.groupid / resp.tp / resp.committed_offset / resp.error
};

auto err = consumer.commit_async(tp, next_offset_to_consume, on_commit);
```

## 3.6 调整消费位点（本地）

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
auto err = consumer.seek(tp, 0);
```

## 3.7 其它常用接口

```cpp
consumer.set_pull_max_record_num_local(100000);
consumer.set_pull_fetch_min_bytes(1024 * 1024);
```

查询本地已消费位置：

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t pos = 0;
auto err = consumer.get_position_consumed(tp, pos);
```

离组：

```cpp
auto err = consumer.leave_group();
```

## 4. 错误处理建议

- 所有 API 返回 `MYMQ_Public::ClientErrorCode` 时，都建议打印 `MYMQ_Public::to_string(err)`。
- 常见状态：
  - `Success`
  - `PULL_TIMEOUT`
  - `EMPTY_RECORD`
  - `NOT_IN_GROUP`
  - `REACHED_MAX_FLYING_REQUEST`
  - `QUEUE_FULL`
  - `NETWORK_FATAL`

## 5. 完整示例位置

- `client/examples/main.cpp`
- `client/examples/example_test_perf.cpp`
- `client/examples/example_test_seek.cpp`
- `client/examples/example_test_backpressure.cpp`

