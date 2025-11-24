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



-----
#### 2.2 Pull Messages

Pulling messages is a **blocking** operation (until timeout or data arrives). Data is returned via a reference parameter, and the function return value indicates the status.

```cpp
// 1. Define the partition you want to pull from
MYMQ_Public::TopicPartition tp("testtopic", 0);

// 2. Prepare a container to receive data
std::vector<MYMQ_Public::ConsumerRecord> res;

// 3. Execute the pull
// Note: The new API does not require passing an offset; the client manages it internally.
auto pull_result = mc.pull(tp, res);

// 4. Check the pull result
if (pull_result == Err_Client::PULL_TIMEOUT) {
    // Pull timed out, no new messages
    std::cout << "pull timeout" << std::endl;
} 
else if (pull_result == Err_Client::NULL_ERROR) {
    // Pull successful, res contains the batch of messages
} 
else {
    // Handle other errors
}
```

#### 2.3 Process Records

Pulling is done in batches. The `pull` interface will fill the passed `std::vector` with multiple messages.

```cpp
// (Continued from previous step)
if (!res.empty()) {
    // Get the first and last message in the batch
    auto& msg_first = res.front();
    auto& msg_back  = res.back();
    
    // Get basic information
    std::cout << "Batch size: " << res.size() << std::endl;
    std::cout << "First Offset: " << msg_first.getOffset() << std::endl;
    std::cout << "Last Offset:  " << msg_back.getOffset()  << std::endl;

    // Iterate to process all messages
    for (const auto& msg : res) {
        std::string key = msg.getKey();
        std::string val = msg.getValue();
        // Business logic...
    }
}
```
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