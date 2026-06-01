# store/maintenance 说明

## 模块职责

`modules/store/maintenance` 承载 store data-plane 的后台维护任务基础设施。

当前只负责：

- `GarbageCollector` task model
- bounded 后台队列
- metadata-driven safety checker gate
- task submit / retry / stop / drain / stats
- 注入式 delete handler 调用边界

当前不负责：

- pending timeout / failed upload / abort cleanup candidate generation
- restart 后继续 cleanup / task persistence / resume
- scrub / repair / rebalance
- metadata / Raft 调用

## 主要文件

- `garbage_collector.h`：GC 任务模型、状态、配置、提交/停机/统计接口
- `garbage_collector.cpp`：任务校验、状态迁移、bounded queue 调度、handler 调用、重试和统计聚合

## T055 固定边界

- `GarbageCollector` 自身不直接调用 metadata / Raft。
- `GarbageCollector` 自身不直接持有 `LocalDiskChunkStore`、`StorageNodeClient`、`MetadataStateMachine` 或 metadata service；metadata facts 和删除执行都必须通过注入 checker / handler 间接完成。
- GC task 必须带 `metadata_boundary`，防止任务模型层无条件表达“直接删除 live chunk”。
- T055 在 delete handler 前增加 metadata-driven safety gate；chunk 仍被 committed live manifest 引用时，不允许调用 delete handler。
- T054 不实现 candidate generation；pending timeout / failed upload / abort cleanup 候选生成留给 T056。
- T054 不实现 restart 后继续 cleanup；任务持久化和 resume 留给 T057。
- retryable failure 当前只做任务状态与 attempts 演进，不做延迟调度器；`next_retry_after_ms` 只保留为扩展点和可观察事实。

## metadata-driven safety check 语义

- safety checker 输入：完整 `GarbageCollectorTask`
  - 包含 `chunk_id`
  - 包含 `reason`
  - 包含 `metadata_boundary`
- safety checker 输出：`GarbageCollectorSafetyCheckResult`
  - `status == kOk` 表示允许执行 delete handler
  - retryable 状态例如 `kNodeUnavailable` / `kTimeout` 表示 metadata facts 临时不可用，可在 `max_attempts` 范围内重试
  - non-retryable 状态例如 `kConflict` 表示 chunk 仍被 committed live manifest 引用，任务直接进入 terminal failed
- safety checker 只决定“是否允许删除”，不负责：
  - 生成 GC task
  - 选择 candidate
  - restart resume
  - metadata / Raft 写回

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

### `GarbageCollectorSafetyCheckResult -> GarbageCollectorAttemptResult` 转换 helper

- 责任：把 metadata safety checker 的返回收口成内部统一 attempt 结果
- 输入：`GarbageCollectorSafetyCheckResult`
- 输出：统一的 `status / error_detail / retry_after_ms`
- 边界：只负责状态转译，不决定 retry 或 terminal state

### live manifest reference 判断 helper（由注入 checker 承担）

- 责任：根据 committed object manifest 事实判断某个 `chunk_id` 是否仍被 live object 引用
- 输入：`chunk_id`、`reason`、`metadata_boundary` 以及 checker 自己持有的 metadata 视图
- 输出：`kOk` 或安全失败状态
- 边界：maintenance 层不直接实现 metadata 扫描逻辑，只要求 checker 保留 `metadata_boundary` 并返回明确结果

### worker loop helper（`SubmitTask(...)` 内部 lambda）

- 责任：驱动单个 task 的 `Queued -> Running -> RetryPending -> Completed/Failed/Cancelled` 状态迁移
- 输入：`task_id`、注入的 metadata safety checker 和 delete handler
- 输出：更新 task snapshot、attempts、last_error、retryable、next_retry_after_ms
- 边界：先过 safety checker，再决定是否调用 delete handler；当前 retry 在同一个 worker 内立即继续，不引入延迟调度器；`kCancelPending` stop 下不会再继续新的 retry

### handler 执行前 gate helper（worker loop 内 safety check）

- 责任：在 delete handler 前执行 metadata-driven safety gate
- 输入：当前 `GarbageCollectorTask`
- 输出：允许删除时继续调用 handler；不允许时直接进入 attempt result 更新
- 边界：若 safety checker 返回非 `kOk`，delete handler 必须完全不被调用

### safety decision 到 task state 的转换 helper（worker loop 内 attempt result 更新）

- 责任：把 safety checker 或 delete handler 的 attempt result 映射到 `Completed / RetryPending / Failed`
- 输入：attempt result、`attempts`、`max_attempts`、stop mode
- 输出：新的 task state 与 `retryable` 标记
- 边界：`kConflict` 等 live-manifest violation 不重试；`kNodeUnavailable` / `kTimeout` 等 retryable safety failure 可重试

### safety failure 的 attempts / last_error / terminal state 处理 helper（worker loop 内 attempt result 更新）

- 责任：在 safety checker 拒绝删除时，仍按一次完整 GC attempt 更新 `attempts`、`last_error`、`last_error_detail` 和 `next_retry_after_ms`
- 输入：safety failure result、当前 task
- 输出：重试或 terminal failed 的任务快照
- 边界：即使 delete handler 没有被调用，safety failure 也算一次 task attempt；repeated blocked task 不会误删 live chunk

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
