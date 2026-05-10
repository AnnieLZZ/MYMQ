# Client Changelog

本文件记录 `client/` 模块的版本变更。

格式约定：
- `## [版本号] - 日期`
- 分组为 `Added / Changed / Fixed / Removed / Performance`

## [Unreleased]

### Added
- 暂无

### Changed
- 暂无

### Fixed
- 暂无

## [4.0.0] - 2025-12-10

### Added
- 对外职责分离：`MYMQ_Producer` 与 `MYMQ_Consumer`。
- 错误码 `UPDATE_GENERATION`（作为状态标识）。
- `TP_Point` 统一绑定分区 `pollbuffer` 与 `endoffset` 指针。

### Changed
- 客户端组协作逻辑改为以心跳与服务端分配为核心。
- `get_position_consumed` 更名为 `get_local_consumed_position`（通过引用返回本地位置）。
- 心跳请求字段更新，支持携带 topic 更新标识与列表。
- 客户端类名由 `MYMQ_clientuse` 调整为 `MYMQ_Consumeruse`。
- 错误码中多个 topic/partition 相关项合并为 `INVALID_TOPIC_PARTITION`。

### Fixed
- 同步修正心跳与 commit offset 的客户端解析流程。

## [3.2.1] - 2025-12-07

### Added
- `NOT_REGISTER` 错误码。

### Changed
- 所有网络请求前增加注册状态检查。

### Fixed
- `pull` 测试重载耗时统计不再包含空等时间。

## [3.2.0] - 2025-12-06

### Added
- `MYMQ_Produceruse`。
- `RecordAccumulator`（对原 push 队列映射的封装）。

## [3.1.0] - 2025-12-06

### Changed
- 删除自动拉取相关参数与逻辑（含 `autopoll_perior_ms`）。
- `pull_bytes` 语义调整为单次请求最小字节配置。
- 增加 `pull` 耗时重载（`int64_t`，单位 us）。

### Fixed
- 心跳世代不一致时触发重入组流程修正。

## [3.0.0] - 2025-12-05

### Changed
- `request_timeout_s` 改为 `request_timeout_ms` 配置。
- `pull` 等待参数调整为毫秒级（`poll_wait_timeout_ms`）。
- 超时管理改用 `Request_timeout_queue`（队列 + 静默删除策略）。

### Performance
- 降低高并发超时检测开销与无效线程压力。

## [2.4.0] - 2025-11-27

### Changed
- 网络 IO 缓冲区改为配置项（默认 16384）。

### Performance
- 降低包体移动开销与后台调度开销。

## [2.2.0] - 2025-11-26

### Added
- `seek` API（不提交 offset 的本地位点调整）。

### Changed
- `pull` 成功返回后自动刷新本地消费位点。
- `ACK_NORESPONCE` 下设置 push 回调将返回 `INVALID_OPRATION`。

## [2.1.0] - 2025-11-25

### Changed
- 调整 ACK 等级为两档：`ACK_NORESPONCE` 与 `ACK_PROMISE_INDISK`。
- `ACK_PROMISE_INDISK` 返回 `topic/partition/offset`。

### Fixed
- 修复 push 回调无效问题。

## [2.0.0] - 2025-11-24

### Changed
- `pull` 接口形态调整（详见 `docs/API_Guide.md`）。

### Performance
- 优化 `pull` 接收解析路径。

## [1.1.0] - 2025-11-23

### Added
- 引入共享线程池分片处理消息路径（按分区哈希）。
