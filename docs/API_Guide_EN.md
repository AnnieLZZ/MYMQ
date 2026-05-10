# MYMQ API User Guide (Current Code)

This guide is based on the current public API in `client/include/MYMQ_C.h`.

## Before You Start

- Config files are loaded only at startup.
- Common client config files:
  - `client/config/communication.ini`
  - `client/config/sys.ini`
  - `client/config/business.ini`

## 1. Headers and Core Types

```cpp
#include "MYMQ_C.h"
#include "MYMQ_PublicCodes.h"
```

Core types:

- `MYMQ_Producer`
- `MYMQ_Consumer`
- `MYMQ_Public::TopicPartition`
- `MYMQ_Public::ClientErrorCode`
- `MYMQ_Public::PushResponce`
- `MYMQ_Public::CommitAsyncResponce`

## 2. Producer Usage

## 2.1 Create Producer

```cpp
// ack_level:
// 1 = wait for server ACK
// 0 = no ACK (callbacks will not fire)
MYMQ_Producer producer("producer-1", 1);
```

## 2.2 Create Topic

```cpp
producer.create_topic("topic_demo", 4);
```

## 2.3 Push Without Callback

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
auto err = producer.push(tp, "k1", "v1");

if (err != MYMQ_Public::ClientErrorCode::Success) {
    // handle error
}
```

## 2.4 Push With Callback

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);

auto on_push = [](MYMQ_Public::PushResponce resp) {
    // resp.tp / resp.errorcode / resp.offset
};

auto err = producer.push(tp, "k2", "v2", on_push);
```

Notes:

- With `ack_level=0`, the server does not send push ACK, so callback will not be called.
- Typical return errors include `QUEUE_FULL`, `NOT_REGISTER`, `TIMEOUT`.

## 2.5 Stop Producer

```cpp
producer.stop();
```

## 3. Consumer Usage

## 3.1 Create Consumer

```cpp
MYMQ_Consumer consumer("consumer-1", 1);
```

## 3.2 Subscribe and Join Group

```cpp
consumer.subscribe_topic("topic_demo");
consumer.join_group("group_demo");
```

Optional checks:

```cpp
bool in_group = consumer.get_is_ingroup();
auto assigned = consumer.get_assigned_partition();
```

## 3.3 Trigger Pull

```cpp
consumer.trigger_pull();
```

## 3.4 Pull Records

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

Pull with latency output:

```cpp
std::vector<MYMQ_Public::ConsumerRecord> records;
int64_t latency_us = 0;
auto err = consumer.pull(records, 5000, latency_us);
```

## 3.5 Commit Offset

Sync commit:

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t next_offset_to_consume = 100;
auto err = consumer.commit_sync(tp, next_offset_to_consume);
```

Async commit (no callback):

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t next_offset_to_consume = 100;
auto err = consumer.commit_async(tp, next_offset_to_consume);
```

Async commit (with callback):

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t next_offset_to_consume = 100;

auto on_commit = [](MYMQ_Public::CommitAsyncResponce resp) {
    // resp.groupid / resp.tp / resp.committed_offset / resp.error
};

auto err = consumer.commit_async(tp, next_offset_to_consume, on_commit);
```

## 3.6 Local Seek

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
auto err = consumer.seek(tp, 0);
```

## 3.7 Other Useful APIs

```cpp
consumer.set_pull_max_record_num_local(100000);
consumer.set_pull_fetch_min_bytes(1024 * 1024);
```

Get local consumed position:

```cpp
MYMQ_Public::TopicPartition tp("topic_demo", 0);
size_t pos = 0;
auto err = consumer.get_position_consumed(tp, pos);
```

Leave group:

```cpp
auto err = consumer.leave_group();
```

## 4. Error Handling

- API calls return `MYMQ_Public::ClientErrorCode`.
- Use `MYMQ_Public::to_string(err)` for readable logs.
- Common values:
  - `Success`
  - `PULL_TIMEOUT`
  - `EMPTY_RECORD`
  - `NOT_IN_GROUP`
  - `REACHED_MAX_FLYING_REQUEST`
  - `QUEUE_FULL`
  - `NETWORK_FATAL`

## 5. Full Examples

- `client/examples/main.cpp`
- `client/examples/example_test_perf.cpp`
- `client/examples/example_test_seek.cpp`
- `client/examples/example_test_backpressure.cpp`
