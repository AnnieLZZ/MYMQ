# MYMQ: High-Performance Distributed Message Queue

> A C++17 message queue project focused on high-throughput data path, low-overhead networking, and production-style concurrency control.
>  
> **Role:** Core Developer | **Focus:** System Design / Network IO / Storage Engine / Concurrency

---

## ⚡ Performance Snapshot

### Single Node, Single Partition (My Benchmark)

**Workload**
- 4,000,000 messages
- Message size: ~200-300B
- End-to-end push + pull + commit path

**Hardware**
- Client: Intel i7-12650H, NVMe SSD
- Server: Intel i7-12650H, NVMe SSD

| Metric | Throughput | Path Definition |
| :--- | :--- | :--- |
| Push (Producer) | ~331,198 msg/s | User API -> Server Page Cache -> ACK -> Client Callback |
| Pull + commit_sync (Consumer) | ~255,983 msg/s | Fetch -> Parse -> Consume -> Commit |

> Notes: Numbers are measured on my local setup and intended to show engineering direction/capability, not universal peak claims.

---

## 🚀 Why This Project Is Interesting

- Designed and implemented a full C++ MQ stack (client + broker) rather than a toy queue API.
- Built around **batch-first data path** to reduce syscall and protocol overhead.
- Implemented **epoll ET + TLS + kTLS sendfile path** on server side.
- Built **log-segment + sparse-index** storage with recovery logic.
- Added **consumer-group generation fencing** and rebalance-aware commit checks.
- Implemented **sharded threading model** on both server and client hot paths.

---

## 🧠 Architecture Highlights

### 1) Network & Transport

- Event loop: `epoll` (ET) + non-blocking TLS.
- Session abstraction: `TcpSession` decouples protocol logic from socket lifecycle.
- Zero-copy attempt path: `SSL_sendfile` when kTLS send is available.
- Protocol: custom binary frame with fixed 12-byte header:
  - `total_length` + `event_type` + `correlation_id` + `ack_level`

### 2) Storage Engine

- Partitioned append-only log by topic-partition.
- Segment files:
  - `.log` for sequential append
  - `.index` for sparse index lookup
- Startup recovery:
  - rebuild index from log
  - truncate corrupted tail entries
- Timed flush and segment maintenance are integrated in storage pipeline.

### 3) Concurrency Model

- Server:
  - Connection events routed by **FD-sharded thread pool**
  - Same FD tends to stay on same worker for locality and lower contention
- Client:
  - Producer batching + ready queue + backpressure
  - Consumer parse path uses parallel processing for pulled batches
- Lock strategy:
  - shared/exclusive split in storage write path
  - reduced critical sections in request handling

### 4) Consumer Group Mechanics

- Group state tracks:
  - `generation_id`
  - `assigned_partitions`
  - `current_holding`
  - `target_assignment`
- Commit is fenced by:
  - member existence
  - generation match
  - partition ownership validation
- Long-poll requests are aware of rebalance/revocation.

---

## 🛠️ Tech Stack

- Language: `C++17`
- Network/IO: `epoll`, non-blocking socket, `OpenSSL`, `kTLS/sendfile`
- Concurrency: `Intel TBB`, sharded thread pool, `moodycamel` queues
- Storage: append log, sparse index, mmap-based index operations
- Compression/Integrity: `ZSTD`, `CRC32`
- Build: `CMake`

---

## 📚 Docs

- API Guide (CN): [`docs/API_Guide.md`](./docs/API_Guide.md)
- API Guide (EN): [`docs/API_Guide_EN.md`](./docs/API_Guide_EN.md)
- Architecture (Code-Fact): [`docs/Architecture_Design.md`](./docs/Architecture_Design.md)

---

## 🚀 Build Quick Start

### Server (Linux)

```bash
cd server
mkdir -p build
cd build
cmake ..
cmake --build .
```

Runtime requirements:
- valid `server.crt` and `server.key` in runtime directory

### Client (Windows / MinGW)

```bash
cd client
mkdir -p build
cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
```

---

## 🧪 Code Entry Points

- Public API: `client/include/MYMQ_C.h`
- Client core: `client/src/MYMQ_Client.cpp`
- Client transport: `client/src/Communication.h`
- Broker core: `server/src/MessageQueue.h`
- Broker network: `server/src/Server.h`
- Storage core: `server/src/Logsegment.h`
- Group coordinator state: `server/include/MYMQ_Server_ns.h`

---

## License

MIT (`LICENSE`)
