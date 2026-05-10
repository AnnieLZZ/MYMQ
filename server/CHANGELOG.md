# Server Changelog

本文件记录 `server/` 模块的版本变更。

格式约定：
- `## [版本号] - 日期`
- 分组为 `Added / Changed / Fixed / Removed / Performance`

## [Unreleased]

### Added
- 新增运行期性能快照接口：`get_perf_snapshot()` / `reset_perf_counters()`。
- 新增性能采样入口：`MyMQ_PerfProbe`（每秒打印一次服务器统计）。

### Changed
- 统一服务端版本头命名为 `MYMQ_ServerVersion.h`。

## [4.0.0] - 2025-12-10

### Removed
- 移除旧的服务端对外 API 层，保留 Broker 进程模式。

### Changed
- 组成员分配逻辑完全收敛到服务端（含一致性哈希分配）。
- 分区管理改为以 TopicPartition 为 key 的扁平化结构。
- 组状态采用目标分配/当前持有等多视图管理，保障消费唯一性。
- offset 归属与查询路径重构，区分组元数据与日志层可信来源。
- `get_first_baseoffset` 相关方法统一更名为 `get_earliestoffset`。
- 去除 Topic 实体类，Topic 仅保留逻辑概念。

### Added
- `GENERATION_EXPIRED` 等世代语义相关错误码与校验流程。
- 反向分区拥有者查询映射与调试辅助接口。
- `PartitionStorage::archive_segment` 等段归档能力。

### Performance
- 调整锁粒度，减少关键路径竞争。
- mmap 管理与移动语义重构，减少重写与资源转移成本。

## [2.0.0] - 2025-11-24

### Changed
- 服务端 push 响应补充 `baseoffset` 字段。

## [1.1.0] - 2025-11-23

### Performance
- 优化日志/索引刷盘策略，降低 `msync` 调用频率。
- 优化网络事件更新路径，降低 `epoll_ctl` 相关开销。
