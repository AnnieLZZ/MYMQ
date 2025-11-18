# MYMQ API User Guide

This document will guide you on how to initialize the client, produce messages, and consume messages.

## Before You Start: Configuration File

> **Important:** All configuration files (e.g., `config/business.ini`) are read only once before the program starts. Any changes made to the configuration file while the program is running will not take effect.

-----

## 1\. Setup & Subscription

### 1.1 Create the Client

First, create an `MYMQ_Client` instance.

```cpp
// Param 1: Client ID (Optional, defaults to "Client-1")
// Param 2: Push ACK Level (Optional, defaults to 1, i.e., ACK_PROMISE_ACCEPT)
std::string clientid = "my-client";
MYMQ::ACK_Level acklevel = MYMQ::ACK_Level::ACK_PROMISE_ACCEPT;

MYMQ_Client mc(clientid, acklevel);
```

  * **ACK Level 1 (`ACK_PROMISE_ACCEPT`)** means the server only confirms that the message has been received and the data is not corrupted.

### 1.2 Subscribe to or Create a Topic

You can subscribe to a topic directly. If the topic or consumer group does not exist, the server will create them automatically.

```cpp
mc.subscribe_topic("testtopic");
```

Alternatively, you can choose to manually create a topic and specify the number of partitions:

```cpp
// Create a topic named "topic1" with 4 partitions
mc.create_topic("topic1", 4);
```

-----

## 2\. Consumer Guide

The following steps will detail the consumer's responsibilities.

> **Core Concept: `TopicPartition`:**
> All operations that require specifying a partition (Pull, Commit, etc.) must use a `MYMQ_Public::TopicPartition` object (referred to as a 'tp object' in this document) to specify the target.

### 2.1 Join a Consumer Group

```cpp
mc.join_group("testgroup");
```

  * Please check the console output for "JoinGroup success".
  * After successfully joining, you can call the `get_assigned_partition()` method to get the list of partitions assigned to you.

### 2.2 Pull Messages

Pulling messages is a **blocking** operation. It will wait for the server to return data.

```cpp
// 1. Define the partition you want to pull from
MYMQ_Public::TopicPartition tp("testtopic", 0);

// 2. Execute the pull (starting from global offset 0)
auto res = mc.pull(tp, 0);

// 3. Check the pull result
if (res.second == Err_Client::NULL_ERROR) {
    // Pull successful
    // res.first is a std::queue<MYMQ::MYMQ_Client::ConsumerRecord>
} else {
    // Pull failed, res.second contains the error code
}
```

### 2.3 Process Records

Pulling is done in batches. A single `pull` may return a queue containing multiple messages.

```cpp
// (Continued from previous step)
std::queue<MYMQ::MYMQ_Client::ConsumerRecord> msg_queue = res.first;

if (!msg_queue.empty()) {
    // Get the first message from the queue
    auto msg1 = msg_queue.front(); // msg1 is a ConsumerRecord object
    
    // Call methods to view message details
    std::string topic = msg1.getTopic();
    size_t partition  = msg1.getPartition();
    size_t offset     = msg1.getOffset();
    std::string key   = msg1.getKey();
    std::string value = msg1.getValue();
    int64_t time      = msg1.getTime();
}
```

### 2.4 Synchronous Offset Commit (Commit Sync)

> **Note:** Before committing manually, please ensure the `'autocommit'` field in the `config/business.ini` file is set to `0` (disabled).

This method will **block** the main thread until the commit times out or a confirmation response is received from the server.

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
size_t new_offset = 100;

mc.commit_sync(tp, new_offset);
```

### 2.5 Asynchronous Offset Commit (Commit Async)

This method **will not** block the main thread.

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
size_t new_offset = 100;

// 1. Simple async commit (don't care about the result)
mc.commit_async(tp, new_offset);

// 2. Async commit (with callback)
// Assuming you have a callback function:
// void MyCommitCallback(const MYMQ_Public::CommitAsyncResponce& resp) { ... }

mc.commit_async(tp, new_offset, MyCommitCallback);
```

### 2.6 Auto Commit

If you don't want to bother with manual commits, you can enable auto-commit:

1.  Set the `'autocommit'` field in the `config/business.ini` file to `1` (enabled).
2.  Also, set the value for `'autocommit_perior_ms'` (e.g., `5000`), which will be the auto-commit interval in milliseconds.

-----

## 3\. Producer Guide

The following steps will detail the producer's responsibilities.

### 3.1 Synchronous Push (Sync Push)

This method will **block** until it receives confirmation from the server (based on the `acklevel` setting). Both Key and Value can be empty.

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
std::string key1 = "key1";
std::string val1 = "val1";

Err_Client err = mc.push(tp, key1, val1);
// Check the err variable to confirm if the push was successful
```

### 3.2 Asynchronous Push (Push with Callback)

This method **will not** block. You can pass a callback function (of type `MYMQ_Public::SupportedCallbacks`) as the 5th parameter to be notified when the push is complete.

```cpp
MYMQ_Public::TopicPartition tp("testtopic", 0);
std::string key1 = "key1";
std::string val1 = "val1";

// Assuming you have a callback function:
// void MyPushCallback(const MYMQ_Public::PushResponce& resp) { ... }

mc.push(tp, key1, val1, MyPushCallback);
```

> **Note:** When `acklevel` is set to `0` (`MYMQ::ACK_Level::NORESPONCE`), the server will not return any response, so the set callback will **never** be triggered.