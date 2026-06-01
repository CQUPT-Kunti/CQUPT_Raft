# store/maintenance 说明

## 模块职责

`modules/store/maintenance` 承载 store data-plane 的后台维护任务基础设施。

当前只负责：

- `GarbageCollector` task model
- bounded 后台队列
- task submit / retry / stop / drain / stats
- 注入式 delete handler 调用边界

当前不负责：

- metadata-driven GC safety check
- pending timeout / failed upload / abort cleanup candidate generation
- restart 后继续 cleanup / task persistence / resume
- scrub / repair / rebalance
- metadata / Raft 调用

## 主要文件

- `garbage_collector.h`：GC 任务模型、状态、配置、提交/停机/统计接口
- `garbage_collector.cpp`：任务校验、状态迁移、bounded queue 调度、handler 调用、重试和统计聚合

## T054 固定边界

- `GarbageCollector` 自身不直接调用 metadata / Raft。
- `GarbageCollector` 自身不直接持有 `LocalDiskChunkStore` 或 `StorageNodeClient`，删除执行只能通过注入 handler 间接完成。
- GC task 必须带 `metadata_boundary`，防止任务模型层无条件表达“直接删除 live chunk”。
- T054 只实现 task model 和 bounded queue，不实现 metadata-driven safety check；真正的 live manifest 保护留给 T055。
- T054 不实现 candidate generation；pending timeout / failed upload / abort cleanup 候选生成留给 T056。
- T054 不实现 restart 后继续 cleanup；任务持久化和 resume 留给 T057。
- retryable failure 当前只做任务状态与 attempts 演进，不做延迟调度器；`next_retry_after_ms` 只保留为扩展点和可观察事实。

## garbage_collector.cpp 关键 helper

### `ValidateTaskForSubmission(GarbageCollectorTask*, const GarbageCollectorConfig&, std::string*)`

- 责任：校验提交任务是否合法，并补齐默认 `max_attempts`
- 输入：待提交 task、collector config
- 输出：合法时返回 `kOk`，非法时返回明确错误并填充错误详情
- 边界：要求 `task_id`、`reason`、`metadata_boundary` 有效，且必须能确定 `chunk_id`

### `ResolveTaskChunkId(GarbageCollectorTask*, std::string*)`

- 责任：统一收口 task 的 `chunk_id` / object identity 解析
- 输入：task 的 `chunk_id`、`object_id`、`version`、`chunk_index`
- 输出：稳定可用的 `chunk_id`
- 边界：若显式 `chunk_id` 与 object identity 同时存在但不一致，返回显式参数错误

### `SubmitTask(GarbageCollectorTask)`

- 责任：提交一个 GC task，登记任务状态并进入 bounded 后台队列
- 输入：调用方提供的 task
- 输出：`GarbageCollectorSubmitResult`
- 边界：队列满返回 overloaded，stop 后返回 stopped，重复 `task_id` 返回 already exists

### `StorageExecutorSubmitCode -> GarbageCollectorSubmitCode` 转换 helper

- 责任：把底层有界执行器的提交结果收口成 GC 自己的 submit result
- 输入：`StorageExecutorSubmitCode`
- 输出：`GarbageCollectorSubmitCode`
- 边界：不发明新的 overloaded/stopped 语义，只做边界转译

### worker loop helper（`SubmitTask(...)` 内部 lambda）

- 责任：驱动单个 task 的 `Queued -> Running -> RetryPending -> Completed/Failed/Cancelled` 状态迁移
- 输入：`task_id` 和注入的 delete handler
- 输出：更新 task snapshot、attempts、last_error、retryable、next_retry_after_ms
- 边界：当前 retry 在同一个 worker 内立即继续，不引入延迟调度器；`kCancelPending` stop 下不会再继续新的 retry

### retry / attempts / last_error 更新 helper（worker loop 内状态更新）

- 责任：根据 handler 返回的 `DeleteChunkResponse` 更新 `attempts`、`last_error`、`last_error_detail`、`retryable`
- 输入：handler response、当前 task、stop mode
- 输出：新的 task state
- 边界：retryable failure 只在 `attempts < max_attempts` 且 `Stop(kCancelPending)` 未阻止时继续；否则进入 terminal failed

### `Drain()`

- 责任：等待当前已提交 task 都进入 terminal state
- 输入：无
- 输出：`GarbageCollectorDrainResult`
- 边界：`Drain()` 不停止 collector；如果调用方继续并发提交新任务，等待时间会被延长

### `Stop(GarbageCollectorStopRequest)`

- 责任：停止接收新任务，并按 `Drain` 或 `CancelPending` 语义收口后台 worker
- 输入：stop mode
- 输出：`GarbageCollectorStopResult`
- 边界：`kDrain` 允许已排队和运行中的 task 走到 terminal state；`kCancelPending` 会取消尚未开始的 queued task，但不会中断已经开始运行的当前 attempt

### `SnapshotStatsLocked(const Impl&)`

- 责任：聚合当前 task states 和累计计数，生成 stats snapshot
- 输入：collector 内部状态
- 输出：`GarbageCollectorStats`
- 边界：completed/failed/cancelled 是当前已记录 task 的终态数量；`submitted_tasks`、`rejected_tasks`、`total_attempts` 是累计计数
