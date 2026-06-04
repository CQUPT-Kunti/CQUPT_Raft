# store/maintenance 说明

## 模块职责

`modules/store/maintenance` 承载 store data-plane 的后台维护任务基础设施。

当前只负责：

- `GarbageCollector` task model
- bounded 后台队列
- metadata-driven safety checker gate
- cleanup candidate generation
- GC task persistence / restart resume
- task submit / retry / stop / drain / stats
- 注入式 delete handler 调用边界
- `ScrubManager` bounded background queue 与 scrub facts 聚合
- `RepairManager` task model、source/target 规划、copy flow 与生命周期状态机
- under-replicated detection 与 under-replicated fact -> repair task 生成

当前不负责：

- rebalance
- metadata / Raft 调用

## T083 fixed boundary

- T083 新增生产 `ScrubManager`，负责 bounded background scrub task queue、task 状态、drain/stop/query 和 scrub facts 聚合。
- `ScrubManager` 只发现 corrupted / lost / under-replicated facts，并产出 repair candidate；不直接 repair，不修改 metadata manifest，不调用 Raft，也不保存 object payload。
- bounded queue 复用独立后台 executor，foreground `ReadChunk` / `WriteChunk` 不经过这条队列；当前 contract 只固定“队列有界、submit 可拒绝、后台任务不无界堆积”，不承诺 OS-level IO priority。
- healthy source 选择仍基于 registry snapshot + `ReplicaPolicySelector` + 本地 checksum 校验；stale/unavailable/unhealthy/high-disk-pressure 副本不会进入 healthy source。
- 当前 `ScrubManager` 不做 task persistence、restart resume、跨节点 `ScrubChunk` fan-out 或 read-side repair。

## T084/T085/T086 fixed boundary

- T084 新增生产 `RepairManager` task model，负责从 scrub/repair candidate 生成 repair task，并固定 source_node、target_node、chunk_id、expected checksum/size、state、progress、attempts、last_error、last_error_detail。
- T085 在此基础上补上最小生产 copy flow：`RunTask()` 通过注入的 `source_reader` / `target_writer` 执行 source read、checksum verify、target durable write 和 task state 推进。
- T086 补上最小生产 under-replicated detection 收口：`ScrubManager` 现在固定 healthy replica count / required replica count / missing replica count，并由 `RepairManager::SubmitUnderReplicatedTask()` 把 completed scrub task 中的 under-replicated fact 转成 repair task。
- `RepairManager` 当前可做 submit / lookup / list / cancel / retry / running / progress / complete / fail / run-task 更新，但不做 metadata manifest coordination、不发布 replica facts、不做 task persistence、read-side repair 或 rebalance。
- `Completed` 只表示 target durable write 已完成；不代表 metadata manifest 已更新，也不代表 source cleanup 已发生。
- `RetryPending` 表示 task 需要下一次 repair attempt；retryable failure 由 source/target 校验、RPC/IO 状态和 target durable 结果共同驱动。
- copy flow 只处理 chunk data-plane payload 和 checksum/size 事实，不调用 metadata / Raft，也不保存 object payload 到 metadata / Raft。
- under-replicated detection 只依赖 scrub manifest、replica checksum/state 事实和 registry snapshot；不直接修改 metadata manifest，不做 read-side repair，也不把 repair completed 伪装成 manifest 已协调。

## 主要文件

- `garbage_collector.h`：GC 任务模型、cleanup candidate 模型、状态、配置、提交/停机/统计接口
- `garbage_collector.cpp`：任务校验、状态迁移、bounded queue 调度、candidate 生成、handler 调用、重试、restart resume 聚合
- `gc_task_store.h`：GC task snapshot 持久化组件接口和 load/save 结果类型
- `gc_task_store.cpp`：GC task snapshot 序列化、反序列化、原子写入和损坏文件处理

## T055 固定边界

- `GarbageCollector` 自身不直接调用 metadata / Raft。
- `GarbageCollector` 自身不直接持有 `LocalDiskChunkStore`、`StorageNodeClient`、`MetadataStateMachine` 或 metadata service；metadata facts 和删除执行都必须通过注入 checker / handler 间接完成。
- GC task 必须带 `metadata_boundary`，防止任务模型层无条件表达“直接删除 live chunk”。
- T055 在 delete handler 前增加 metadata-driven safety gate；chunk 仍被 committed live manifest 引用时，不允许调用 delete handler。
- T054 不实现 restart 后继续 cleanup；任务持久化和 resume 留给 T057。
- retryable failure 当前只做任务状态与 attempts 演进，不做延迟调度器；`next_retry_after_ms` 只保留为扩展点和可观察事实。

## T056 fixed boundary

- T056 为 pending timeout、failed upload、abort cleanup、deleted object cleanup 新增 generic `CleanupCandidate` 生产 helper。
- candidate generation 只负责把已有 metadata facts / durable chunk facts 规范化为待清理候选，不直接提交 GC task，也不直接删除 chunk。
- candidate 生成后仍必须经过：
  - `CleanupCandidateToGarbageCollectorTask(...)`
  - `GarbageCollector` submit / worker loop
  - metadata-driven safety checker
  - 注入式 delete handler
- T056 不实现 restart resume、cleanup persistence、延迟调度器，也不让 maintenance 层自己扫描 metadata / Raft。

## T057 fixed boundary

- T057 为 `GarbageCollector` 新增最小 snapshot persistence 和 restart resume。
- 持久化内容覆盖：
  - `task_id`
  - `chunk_id`
  - `object_id`
  - `version`
  - `chunk_index`
  - `reason`
  - `metadata_boundary`
  - `attempts`
  - `max_attempts`
  - `last_error`
  - `last_error_detail`
  - `state`
  - `retryable`
  - `next_retry_after_ms`
- 恢复策略固定为：
  - `Queued` / `RetryPending` 恢复后继续可执行
  - `Running` 恢复后规范化为可重新执行状态，不永久卡死
  - `Completed` / `Failed` / `Cancelled` 恢复后只保留状态，不自动重跑
- snapshot 写入通过 `DurableFile` staging + publish + directory sync 完成，避免半写文件直接污染恢复。
- T057-FIX 把 snapshot 保存改成 streaming append writer：先排序，再写 header/count，随后逐 task 生成单行并立即 `Append`，不再先在内存中拼整份 payload。
- T057 不实现 delayed retry scheduler、repair / rebalance / scrub，也不让 persistence 层调用 metadata / Raft。

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

### `BuildMetadataBoundary(CleanupCandidateSource, ...)`

- 责任：根据 candidate source、bucket/object/object_id/version/deadline 生成稳定 `metadata_boundary`
- 输入：candidate 来源和生成时的 metadata 事实
- 输出：供 safety checker / task snapshot 继续传递的边界字符串
- 边界：只表达生成时的 metadata fact，不等价于 restart persistence 或实时 metadata snapshot

### `NormalizeCleanupChunkFact(CleanupChunkFact*, const std::string&, std::uint64_t)`

- 责任：校验和补齐单个 durable chunk fact 的 `object_id/version/chunk_id`
- 输入：原始 chunk fact、目标 object identity
- 输出：规范化后的 chunk fact
- 边界：显式 `chunk_id` 与 object identity 不一致时直接丢弃，避免把错误 candidate 送入 GC

### `NormalizeCleanupCandidates(std::vector<CleanupCandidate>)`

- 责任：对生成好的 cleanup candidates 做稳定排序和去重
- 输入：同一轮生成的 candidate 列表
- 输出：按 `chunk_index -> offset -> chunk_id -> bucket -> object_key -> source` 排序后的唯一列表
- 边界：当前去重 key 只覆盖 `bucket/object_key/chunk_id/source`，不做跨轮次持久化去重

### `BuildCleanupCandidates(...)`

- 责任：把统一输入事实转换成 `CleanupCandidate`
- 输入：candidate source、object state、cleanup reason、metadata 基本事实、durable chunks
- 输出：携带 `reason/object identity/chunk identity/metadata_boundary` 的 generic cleanup candidates
- 边界：这里只做生成，不决定是否允许删除 live chunk

### `BuildPendingTimeoutCleanupCandidates(const PendingTimeoutCleanupRequest&)`

- 责任：为超时 `PENDING` object 生成 orphan cleanup candidates
- 输入：pending object metadata facts、`created_at`、`now`、`timeout_ms`、durable chunks
- 输出：超时后可提交 GC 的 candidate 列表
- 边界：未超时、`timeout_ms == 0`、非 `PENDING` object 一律不生成

### `BuildFailedUploadCleanupCandidates(const FailedUploadCleanupRequest&)`

- 责任：为 failed upload 的 durable chunks 生成 cleanup candidates
- 输入：object metadata facts 和失败后残留的 durable chunk facts
- 输出：`kFailedUploadCleanup` candidates
- 边界：已 committed object 不生成 failed-upload cleanup candidates

### `BuildAbortCleanupCandidates(const AbortCleanupRequest&)`

- 责任：为 abort cleanup 生成 candidates
- 输入：aborted/deleted object facts 和 durable chunk facts
- 输出：`kAbortCleanup` candidates
- 边界：只接受 `Aborted` 或 `Deleted` object state，不把 committed object 误当作 abort cleanup

### `BuildDeletedObjectCleanupCandidates(const DeletedObjectCleanupRequest&)`

- 责任：为 metadata tombstone / deleted object 生成 deleted-object cleanup candidates
- 输入：deleted object facts 和 durable chunk facts
- 输出：`kDeletedObjectCleanup` candidates
- 边界：只接受 `Deleted` object state；live object manifest 保护仍由 safety checker 决定

### `CleanupCandidateToGarbageCollectorTask(const CleanupCandidate&)`

- 责任：把 generic cleanup candidate 转成 GC task
- 输入：规范化后的 `CleanupCandidate`
- 输出：携带 `task_id/chunk_id/object_id/version/chunk_index/reason/metadata_boundary` 的 `GarbageCollectorTask`
- 边界：只做字段映射，不做提交、持久化或 safety decision

### `ValidateRecoveredTask(GarbageCollectorTask*, std::string*)`

- 责任：校验从持久化 snapshot 读回的 task 是否具备恢复所需的最小字段
- 输入：已反序列化 task
- 输出：合法时返回 `kOk`
- 边界：允许 terminal task 的 `attempts == max_attempts`，但禁止缺失 `task_id/metadata_boundary/reason/max_attempts/chunk identity`

### `NormalizeRecoveredTaskState(GarbageCollectorTask*)`

- 责任：把持久化前的 task state 规范化成 restart 后的可执行或终态
- 输入：恢复出的 task
- 输出：规范化后的 `state/retryable/next_retry_after_ms`
- 边界：`Running` 不会被原样保留，避免 restart 后永久卡死

### `ResolveTaskChunkId(GarbageCollectorTask*, std::string*)`

- 责任：统一收口 task 的 `chunk_id` / object identity 解析
- 输入：task 的 `chunk_id`、`object_id`、`version`、`chunk_index`
- 输出：稳定可用的 `chunk_id`
- 边界：若显式 `chunk_id` 与 object identity 同时存在但不一致，返回显式参数错误

### `SubmitTask(GarbageCollectorTask)`

- 责任：提交一个 GC task，登记任务状态并进入 bounded 后台队列
- 输入：调用方提供的 task
- 输出：`GarbageCollectorSubmitResult`
- 边界：队列满返回 overloaded，stop 后返回 stopped，重复 `task_id` 返回 already exists；启用 persistence 时，只有 snapshot 落盘成功后才算真正接收 task

### restart resume helper（构造阶段 load + resume）

- 责任：启动时加载 persisted snapshot、规范化状态并重新调度可恢复任务
- 输入：`GarbageCollectorTaskStore::LoadSnapshot()` 结果
- 输出：内存 `tasks` map 和待恢复执行的 task 列表
- 边界：load 失败不会让进程崩溃；恢复后的 task 仍必须重新经过 safety checker 和 delete handler

### completed / failed / retry pending 恢复策略 helper

- 责任：固定不同 task state 的恢复语义
- 输入：persisted task state
- 输出：是否重跑、是否保留终态
- 边界：
  - `Completed/Failed/Cancelled`：仅恢复事实，不自动重跑
  - `RetryPending`：保留上一次错误事实并继续可执行
  - `Running`：转换为可重新调度状态

### persistence load / save helper（`GarbageCollectorTaskStore::LoadSnapshot/SaveSnapshot`）

- 责任：读写整份 GC task snapshot
- 输入：task 列表或 snapshot 文件
- 输出：`LoadResult` / `DurableFileResult`
- 边界：当前采用 whole-snapshot 重写，不做增量 WAL 或多版本保留；保存路径已改成 streaming append，但最终磁盘上仍只保留一份完整 `gc/tasks.snapshot`

### task serialization / deserialization helper（`gc_task_store.cpp`）

- 责任：把 task snapshot 编码/解码成带 schema header 的稳定文本格式
- 输入：task 字段与原始 snapshot 行
- 输出：序列化字符串或反序列化后的 task
- 边界：字符串字段采用 hex 编码，空串使用显式占位，避免空字段破坏解析；单 task 行仍保持 `GC_TASK_STORE_V1` 兼容格式

## scrub_manager.cpp 关键 helper

### `ValidateTaskForSubmission(ScrubTask*, const ScrubManagerConfig&, std::string*)`

- 责任：校验 scrub task 的 `task_id`、manifest 和默认 timeout，并规范化初始状态。
- 输入：待提交 task、manager config。
- 输出：合法时返回 `kOk`，非法时返回明确错误。
- 边界：要求 `replica_nodes` 非空，且 manifest 必须能收口到稳定 `chunk_id`。

### `ResolveManifestIdentity(ScrubManifest*, std::string*)`

- 责任：统一收口 manifest 的 `chunk_id` / object identity。
- 输入：manifest 中的 `chunk_id/object_id/version/chunk_index`。
- 输出：稳定可用的 `identity.chunk_id`。
- 边界：显式 `chunk_id` 与 object identity 不一致时返回参数错误。

### `InspectReplica(ChunkStore*, const StorageNodeId&, const ScrubManifest&, std::size_t)`

- 责任：对单副本执行 `StatChunk` + checksum verify，产出副本事实。
- 输入：副本对应 `ChunkStore`、node_id、manifest 和副本序号。
- 输出：单副本 `ScrubReplicaFact`。
- 边界：只做本地 checksum/state 检查，不触发 repair。

### `IsHealthyReplicaForRepairSource(...)`

- 责任：统一判断某个 scrub 副本事实是否能计入 healthy replica count 并作为 repair source。
- 输入：单副本 `ScrubReplicaFact`、对应 registry snapshot。
- 输出：是否计入 healthy replica/source。
- 边界：checksum verified、非 corrupted/missing、registry liveness live、health healthy、disk pressure 非 high/full 才算 healthy；stale/unavailable/unhealthy 副本不计入。

## repair_manager.cpp 关键 helper

### `ValidateExecutionSourceSnapshot(...)` / `ValidateExecutionTargetSnapshot(...)`

- 责任：在真正执行 copy 前重新校验 source/target 的 registry snapshot。
- 输入：source/target 节点快照、target 期望容量。
- 输出：`OK` 或明确的 `NODE_UNAVAILABLE/OVERLOADED/DISK_FULL/CONFLICT` 等错误。
- 边界：只依赖运行时 registry snapshot；不做 metadata manifest freshness 协调。

### `RepairManager::RunTask(std::string_view)`

- 责任：驱动单个 repair task 的最小生产 copy flow。
- 输入：task id。
- 输出：`RepairTaskRunResult`，包含 source/target、source checksum/size、retryable、最终 task snapshot。
- 边界：顺序固定为 source/target revalidate -> source read -> checksum/size verify -> target durable write -> task state update；只有 target durable 成功才 `CompleteTask()`，否则进入 `Failed` 或 `RetryPending`；不更新 metadata manifest，不做 source cleanup。

### `RepairTaskSourceReader` / `RepairTaskTargetWriter`

- 责任：把 source read 和 target durable write 抽象成可注入生产路径。
- 输入：`RepairTask`、`StorageTaskContext`，以及 target writer 的 payload/checksum。
- 输出：source 侧的 payload/checksum/state/verified 事实，或 target 侧的 durable/already_exists/retryable 结果。
- 边界：允许通过真实 `StorageNodeClient::ReadChunk/RepairChunk` 或等价生产路径接线；maintenance 层不硬编码 metadata / Raft / manifest 更新。

### `PlanRepairTask(...)`

- 责任：收口 repair candidate、registry snapshot 和 placement 结果，规划稳定的 source_node / target_node。
- 输入：`RepairTaskRequest`、registry、`now_unix_ms`。
- 输出：source/target 已固定的 planning result，或明确的 invalid/no-source/no-target 边界。
- 边界：只做 under-replicated/repair candidate -> task 规划；不执行 copy，不修改 metadata manifest。

### `RepairManager::SubmitUnderReplicatedTask(const ScrubTask &)`

- 责任：把 completed scrub task 中的 under-replicated fact 转成生产 repair task。
- 输入：带 `repair_candidate` 的 completed scrub task。
- 输出：`UnderReplicatedTaskSubmitResult` 和可选 `RepairTask`。
- 边界：只接受 `under_replicated=true` 且存在 healthy source 的 scrub 结果；`lost_or_unrecoverable`、`no healthy source`、`no healthy target` 都返回明确状态；重复扫描生成的同一 plan 走稳定 task id 幂等。
- 输出：`ScrubReplicaFact`。
- 边界：只读本地 chunk facts；发现 checksum mismatch/corruption 时可沿用现有 quarantine 语义，但不触发 repair。

### `RunDefaultScrubTask(const ScrubTask&, const std::map<StorageNodeId, ChunkStore*>&, const StorageNodeRegistry*, std::uint64_t)`

- 责任：执行默认生产 scrub 逻辑，聚合 replica facts、healthy sources 和 repair candidate。
- 输入：scrub task、store map、registry snapshot source、当前时间。
- 输出：`ScrubTaskResult`。
- 边界：只产出 facts/candidate；不修改 metadata manifest，不调用 Raft，不落 payload。

### `SubmitTask(ScrubTask)`

- 责任：把 scrub task 放入 bounded background queue，并登记任务状态。
- 输入：调用方提供的 task。
- 输出：`ScrubManagerSubmitResult`。
- 边界：队列满返回 overloaded，stop 后返回 stopped，重复 `task_id` 返回 already exists。

### `Drain()`

- 责任：等待当前已提交 scrub task 收口。
- 输入：无。
- 输出：`ScrubManagerDrainResult`。
- 边界：当前只等待内存中 queued/running 任务，不负责持久化 task 恢复。

### `Stop(ScrubManagerStopRequest)`

- 责任：停止接收新任务，并按 `Drain` 或 `CancelPending` 语义关停后台队列。
- 输入：stop mode。
- 输出：`ScrubManagerStopResult`。
- 边界：`CancelPending` 只取消尚未开始的 queued task；正在运行的 task 允许自然跑完。

## repair_manager.cpp 关键 helper

### `ValidateSubmitRequest(RepairTaskRequest*, const StorageNodeRegistry*, StorageNodeId*, StorageNodeId*, std::string*, std::uint64_t)`

- 责任：校验 repair task request，收口 manifest/candidate，并固定 source_node 与 target_node。
- 输入：repair task request、registry、当前时间。
- 输出：合法时返回 `kOk` 并写出选中的 source/target。
- 边界：要求 chunk_id、expected checksum、expected size、healthy source candidate 都存在；不执行 copy，不写 chunk。

### `SelectSourceNode(...)`

- 责任：从 repair candidate 的 healthy source 列表里选出当前可用 source。
- 输入：source candidates、registry snapshots。
- 输出：选中的 `source_node`。
- 边界：只接受 live + healthy + 非高/full disk pressure 的 source；不读取 payload，不做 checksum copy。

### `SelectTargetNode(const ScrubManifest&, const StorageNodeRegistry&, std::uint64_t, StorageNodeId*, std::string*)`

- 责任：基于 manifest 和 registry facts 用 `PlacementManager` 规划 target。
- 输入：manifest、registry、当前时间。
- 输出：选中的 `target_node`。
- 边界：只做规划，不执行 target durable write；target 仍受 placement 的 health / overload / capacity 过滤。

### `BuildRepairTaskId(...)`

- 责任：为 repair task 生成稳定、幂等的 `task_id`。
- 输入：`chunk_id`、expected checksum/size、source_node、target_node。
- 输出：稳定 task_id。
- 边界：同一规划结果重复提交会命中相同 task_id，并返回 already exists。

### `SubmitTask(const RepairTaskRequest&)`

- 责任：从 repair candidate 创建 repair task 并写入 task registry。
- 输入：repair task request。
- 输出：`RepairManagerSubmitResult`。
- 边界：active queue 满或 task registry 满返回 overloaded；重复 task_id 返回 already exists；只创建 task，不启动 copy。

### `MarkTaskRunning(std::string_view)`

- 责任：把 task 从 `Queued/RetryPending` 推进到 `Running`，并记一次 attempt。
- 输入：task_id。
- 输出：更新后的 task snapshot。
- 边界：不执行真实 copy；attempts 只在进入 running 时增加。

### `UpdateTaskProgress(std::string_view, std::uint32_t)`

- 责任：更新 running task 的 progress。
- 输入：task_id、`0..100` 的 progress。
- 输出：更新后的 task snapshot。
- 边界：只改 task model；completed progress 仍必须通过 `CompleteTask(...)` 收口。

### `FailTask(std::string_view, StorageNodeStatusCode, std::string, bool, std::uint64_t)`

- 责任：记录 repair failure，并根据 retryable 边界落到 `Failed` 或 `RetryPending`。
- 输入：task_id、错误码、错误详情、是否可重试、retry_after_ms。
- 输出：更新后的 task snapshot。
- 边界：只记录失败事实，不代表 target durable 写入或 metadata 更新发生过。

### `RetryTask(std::string_view)`

- 责任：把 `Failed` task 重新推进到 `RetryPending`。
- 输入：task_id。
- 输出：更新后的 task snapshot。
- 边界：retry 只改变 task model；不会自动写 target chunk，也不会自动变成 completed。

### `CancelTask(std::string_view)`

- 责任：取消尚未执行的 `Queued/RetryPending` task。
- 输入：task_id。
- 输出：更新后的 task snapshot。
- 边界：running/completed/failed task 当前返回 conflict，不做运行中中断。

### `CompleteTask(std::string_view)`

- 责任：把 running task 标记为 completed，并把 progress 收口到 100。
- 输入：task_id。
- 输出：更新后的 task snapshot。
- 边界：只更新 task model，不代表 metadata manifest 已协调或 target durable copy 已完成。

### task state normalization helper（`NormalizeRecoveredTaskState`）

- 责任：统一 restart 后 `Queued/Running/RetryPending/Completed/Failed/Cancelled` 的行为
- 输入：恢复出的 task state
- 输出：可提交调度的状态或终态
- 边界：不实现 delayed retry scheduler，只决定“能不能立刻恢复执行”

### atomic write / publish helper（`GarbageCollectorTaskStore::SaveSnapshot`）

- 责任：通过 `OpenStagingWriter -> Append(header/count/task-line) -> Flush -> Close -> PublishStagedFile -> SyncDirectory` 原子更新 snapshot
- 输入：按稳定排序遍历的 task snapshot 数据流
- 输出：到 durable boundary 的 snapshot 文件
- 边界：Windows directory sync 仍可能返回 explicit unsupported，需要后续 Windows 实机验证

### corrupted persistence file 处理 helper（`GarbageCollectorTaskStore::LoadSnapshot`）

- 责任：识别 magic/count/task-line 损坏并返回明确错误
- 输入：snapshot 文件内容
- 输出：`kCorrupted` 或明确 `error_detail`
- 边界：损坏 snapshot 不导致 collector 崩溃；当前策略是报告错误并放弃恢复该 snapshot

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
