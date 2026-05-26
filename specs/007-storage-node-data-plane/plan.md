# Implementation Plan: Storage Node Data Plane

**Branch**: `007-storage-node-data-plane` | **Date**: 2026-05-25 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `specs/007-storage-node-data-plane/spec.md`

## Summary

本计划在已经完成 metadata-only Raft control-plane 与 `006-remove-kv-metadata-state-machine` no-KV 收口的 CQUPT_Raft 项目上，规划新增 StorageNode chunk/data-plane。007 不是新建项目，不重写 Raft，不恢复旧 KV demo，也不把对象字节写入 Raft log。

规划路线采用分阶段推进：先复核当前 metadata/control-plane 与 no-KV 边界，再实现 LocalDiskChunkStore MVP 和独立跨平台 durable file 能力，之后才引入 StorageNode RPC、最小上传/读取/删除闭环、高并发 backpressure、heartbeat/registry、Placement、GC、Scrub/Repair、Rebalance 与 Windows/Linux 验证矩阵。Repair/Rebalance 等工业化能力保留清晰契约和扩展点，但不在第一阶段一次性实现。

本 `/speckit-plan` 阶段只生成规划文档，不创建 `tasks.md`，不修改生产源码、proto、CMake、测试实现或 `006` 既有收口文档。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: gRPC、Protobuf、GoogleTest、CMake、标准库、`std::filesystem`、线程/锁原语、平台文件 API  
**Storage**: 现有 Raft log/snapshot 继续由 `modules/raft/storage` 负责；StorageNode chunk 数据必须规划独立 LocalDiskChunkStore 和 durable file abstraction，不复用 Raft log/snapshot 语义替代 chunk durability。  
**Testing**: GoogleTest + CTest + `test.sh` / `test.ps1`；storage 高并发测试可并行，recovery/snapshot/catch-up/crash 边界测试低并发，必要时 `CTEST_PARALLEL_LEVEL=1`。  
**Target Platform**: Linux 为主要验证环境；Windows 必须有 FlushFileBuffers、MoveFileEx/ReplaceFile、long path、UTF-8 path、权限和磁盘错误语义；macOS 不作为首轮验证主平台但不能被 shared code 排除。  
**Project Type**: 现有 Raft metadata/control-plane 上的对象存储 data-plane 增量工业化，不是 greenfield 分布式存储重写。  
**Performance Goals**: MVP 至少规划每个 StorageNode 支持 100 个并发 chunk 操作的压力验证；所有请求队列、IO 队列、worker pool、executor 都必须有边界、backpressure、timeout、cancellation 和 rate limit。  
**Constraints**: 保留 Raft 选举、复制、commit/apply、snapshot、restart recovery、follower catch-up；不修改当前阶段生产代码、proto、CMake、tests；不改变已有 public API 行为、持久化格式或协议语义。  
**Scale/Scope**: 新增 StorageNode 数据面规划，最小对接现有 metadata manifest；不恢复 KV，不实现 erasure coding，不实现完整多版本/generation，只保留扩展点。

## Existing Baseline

- `RaftNode` 默认构造当前装配 `MetadataStateMachine`，并通过 `RaftNode::ProposeMetadata()` 提交 metadata 写请求。
- `RaftNode::InitServer()` 当前注册 `RaftService` 与 `MetadataService`，未发现 `KvService` 注册。
- `MetadataService` 已提供 bucket/object 生命周期 RPC：`CreateBucket`、`DeleteBucket`、`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject`、`HeadObject`、`ListObjects`。
- `ObjectRecord` 已有 `bucket`、`object_key`、`object_id`、`version`、`size`、`etag`、`state`、`chunks`、`create_time`、`commit_time`、`delete_time`。
- `ChunkRef` 已有 `chunk_id`、`offset`、`size`、`replica_nodes`、`checksum`，可作为第一阶段 manifest 兼容载体。
- `MetadataStateMachine` 已实现 `CreateObject -> PENDING`、`CommitObject -> COMMITTED`、`AbortObject/DeleteObject -> DELETED/tombstone`，并维护 `chunk_ref_index_`。
- request_id 幂等已有两层：`RaftNode` 侧 in-flight/completed proposal 去重，以及 `MetadataStateMachine` 侧 `request_fingerprints_` 持久化去重。
- metadata snapshot V2 已覆盖 bucket、object、object_index、chunk_ref_index、request、tombstone、applied boundary。
- Raft log/snapshot 持久化已有 checksum、fsync/FlushFileBuffers、staging/rename/publish，但该能力属于 Raft metadata 持久化，不是 StorageNode chunk 数据落盘。
- 窄范围检索未发现真实 `StorageNode`、`PlacementManager`、`RepairManager`、`RebalanceManager`、`ScrubManager`、`WriteChunk`、`ReadChunk`、`DeleteChunk` 数据面实现。
- no-KV 检索命中主要来自 `tests/no_kv_surface_audit.cmake` 和 metadata delete 语义；未发现旧 `KvStateMachine`、`KvService`、`raft_kv_client` 生产主路径。

## Architecture Boundaries

- Raft 只负责 metadata/control-plane。
- StorageNode 负责 chunk/data-plane。
- `MetadataStateMachine` 不保存 chunk 字节。
- Raft log 不写 object payload。
- StorageNode 不决定 object 是否 committed。
- metadata 是 object state 和 chunk manifest 的 source of truth。
- 上传流程必须是 metadata pending -> chunk durable -> metadata commit。
- 读取流程必须是 read metadata first -> read StorageNode replicas。
- 删除流程必须是 metadata tombstone/DELETED -> background async chunk cleanup。
- 007 是新增 StorageNode 数据面，不是替换已有 metadata control-plane。

## No-KV Hard Constraints

007 后续任何 plan/tasks/implementation 都必须保持以下硬约束：

- 不得重新引入 `CommandType::kSet`。
- 不得重新引入 `CommandType::kDelete`。
- 不得重新引入 `KvStateMachine`。
- 不得重新引入 `KvService`。
- 不得重新引入 `raft_kv_client`。
- 不得重新引入 `DebugGetValue`。
- 不得恢复 KV proto。
- 不得恢复 KV target。
- 不得恢复 KV fallback。
- 不得恢复 KV regression-only path。
- tests 中不得使用 `SetCommand` / `DeleteCommand` / KV 状态机断言。
- 不得把 StorageNode 设计成 KV。
- 不得把对象数据写入 Raft log。
- 不得回滚或绕过 006 no-KV 收口成果。

## Constitution Check

*GATE: Passed before Phase 0 research. Re-check after Phase 1 design.*

- Verified existing capabilities affected by this feature are identified and excluded from unnecessary replanning.  
  通过。Raft 选举、复制、commit/apply、snapshot、restart recovery、follower catch-up、metadata snapshot V2、metadata request_id 幂等均作为已完成基线保护；007 只规划新增 chunk/data-plane。

- Any protocol, public API, or persisted format change is either absent or explicitly justified with migration and regression coverage.  
  通过。本 plan 阶段不修改真实 proto、public API、生产持久化格式或测试实现。StorageNode RPC 仅写 draft contract；未来若落地 proto 扩展，必须单独任务同步调用方、CMake target 和测试。

- Durability, crash-recovery, and restart-recovery implications are stated for every affected path in `node`, `replication`, `storage`, or `state_machine`.  
  通过。007 不改 Raft durability；StorageNode chunk durability 独立规划 staging、checksum、file fsync/fdatasync、FlushFileBuffers、atomic publish、parent directory sync、restart index rebuild、corrupted quarantine。

- Linux-specific validation is explicitly labeled, and Windows/macOS fallback, adaptation, or deferred follow-up is recorded.  
  通过。Phase 2 与 Phase 11 单独规划 Linux/Windows durability 差异；required durability operations 不允许 no-op success。

- Test entry points are defined through CTest plus any justified platform-specific script or preset additions.  
  通过。plan 只规划未来测试入口，不修改 `test.sh`、`test.ps1`、CMake 或 tests。

- Observability and diagnostics impact is captured for high-risk work.  
  通过。StorageNode heartbeat、capacity、load、health、IO error、disk pressure、GC/repair/rebalance progress 均作为未来可观察事实规划。

**Post-Design Re-check**: 通过。设计产物仍保持 no-code、no-proto、no-CMake、no-tests 修改；新模块路径为建议，不代表本阶段落地。

## Project Structure

### Documentation (this feature)

```text
specs/007-storage-node-data-plane/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── cross-task-risk-notes.md
├── contracts/
│   ├── storage-node-api.md
│   ├── storage-node.proto.draft
│   ├── cross-platform-durability-contract.md
│   ├── placement-contract.md
│   └── gc-repair-rebalance-contract.md
└── tasks.md                    # NOT created by /speckit-plan
```

### Suggested Future Source Layout

本阶段不创建生产源码。后续实现可按如下路径规划，最终以 `/speckit-tasks` 拆分结果为准：

```text
modules/raft/storage_node/
├── chunk_store.h / .cpp
├── local_disk_chunk_store.h / .cpp
├── chunk_index.h / .cpp
├── durable_file.h / .cpp
├── storage_node.h / .cpp
├── storage_node_client.h / .cpp
├── storage_node_service.h / .cpp
├── placement_manager.h / .cpp
├── replica_policy.h / .cpp
├── garbage_collector.h / .cpp
├── scrub_manager.h / .cpp
├── repair_manager.h / .cpp
└── rebalance_manager.h / .cpp
```

### Existing Modules And Modification Boundaries

- `modules/raft/node`: 只允许未来做最小 metadata/data-plane 对接或 coordinator hook；不得改选举、复制、commit/apply、snapshot/recovery 语义。
- `modules/raft/state_machine`: 只保存 metadata manifest，不保存 chunk 字节；未来仅在 manifest/repair/rebalance metadata 更新需要时做明确任务。
- `modules/raft/service`: 现有 `MetadataService` 保持 control-plane；StorageNode service 应独立边界，不回退 KV。
- `modules/raft/storage`: 继续只负责 Raft hard state、segment log、snapshot catalog；StorageNode 不应直接复用该模块的 Raft log/snapshot publish 作为 chunk durability。
- `modules/raft/metadata` 和 `modules/raft/common`: 保持 `ObjectRecord.chunks` / `ChunkRef` 兼容；未来若扩展字段，必须明确 proto/metadata 契约影响。
- `proto`: 本阶段只生成 draft contract；未来真实 proto 扩展需要单独任务，避免与 `metadata.proto` / `common.proto` 边界冲突。
- `tests`: 本阶段只规划测试，不新增、不删除、不跳过测试。

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|--------------------------------------|
| 新增 `modules/raft/storage_node` 数据面边界 | StorageNode 负责真实 chunk IO、durability、repair/rebalance participation，与 Raft metadata storage 职责不同 | 复用 `modules/raft/storage` 会混淆 Raft log/snapshot 持久化与 chunk 数据落盘，增加误写 object payload 到 Raft 的风险 |
| 新增跨平台 durable file abstraction | chunk 文件需要独立定义 fsync/fdatasync、FlushFileBuffers、atomic publish、directory sync、path normalization、错误分类 | 直接调用现有 snapshot publish helper 会绑定 Raft snapshot 目录语义，无法表达 chunk staging/index/quarantine/restart rebuild |
| StorageNode heartbeat 独立于 Raft heartbeat | capacity、used、available、chunk_count、disk pressure、IO error、load 不属于 Raft leader election heartbeat | 复用 Raft heartbeat 无法承载数据面健康和容量事实，会导致 Placement/Repair/Rebalance 盲选节点 |

## Cross-Platform Durability And Validation Matrix

跨平台要求不是附属项，而是 StorageNode data-plane 的硬门槛。007 后续实现任何 chunk 文件持久化能力时，都必须先满足下表中的 contract 或返回明确错误；不允许在任一平台用 no-op success 伪装 durability。

| Area | Linux Requirement | Windows Requirement | Validation Requirement |
|------|-------------------|---------------------|------------------------|
| File open/write | 使用明确的二进制文件写入路径，处理短写、`EINTR`、`ENOSPC`、`EACCES`、`EIO` | 使用 Windows file handle，处理 sharing mode、短写、`ERROR_DISK_FULL`、`ERROR_ACCESS_DENIED`、IO error | partial write、disk full、permission denied、IO error 单测/故障注入 |
| Data flush | staging 文件 publish 前执行 `fdatasync` 或 `fsync` | staging 文件 publish 前执行 `FlushFileBuffers` | crash before flush / flush failure 不得产生 live chunk |
| Atomic publish | staging -> final 使用 same-filesystem rename 语义 | staging -> final 使用 `MoveFileEx` / `ReplaceFile` 风险受控语义 | crash after flush before rename、publish conflict、target exists |
| Parent directory durability | final rename 后 sync parent directory；失败返回明确错误 | 如无法提供等价 directory durability，必须记录较弱 contract 或返回 explicit unsupported/error | crash after rename before directory sync，restart index rebuild 验证 |
| Path handling | `std::filesystem` + normalized relative chunk path，拒绝 path traversal | UTF-8 path、Windows long path、reserved names、separator normalization | UTF-8、long path、非法路径、跨目录 publish 拒绝 |
| Staging cleanup | 重启扫描清理 stale/incomplete staging | 同 Linux，并处理 Windows 文件占用/handle 未释放 | stale staging cleanup，process restart 后不进入 live index |
| Partial write detection | size/checksum 不一致进入 failed/quarantine，不 publish | 同 Linux | partial write、checksum mismatch、corrupted quarantine |
| Error classification | `disk_full`、`permission_denied`、`io_error`、`timeout`、`unsupported` 明确分类 | 映射 Windows error code 到同一错误分类 | API response 和 health/error count 验证 |
| Restart recovery | 扫描 final/staging/quarantine/deleting，重建 ChunkIndex | 同 Linux，额外覆盖 long path 和 sharing mode 残留 | restart index rebuild，corrupted chunk quarantine |
| Concurrency | per-chunk lock、sharded ChunkIndex、bounded IO queue | 同 Linux，不依赖 POSIX-only primitive | high-concurrency chunk IO，可并行 storage tests |

后续 `/speckit-tasks` 应把跨平台验证拆为可执行测试项：

- Linux：`fsync` / `fdatasync`、directory sync、disk full、permission denied、partial write、staging cleanup、atomic publish、checksum mismatch、crash recovery、restart index rebuild、高并发 chunk IO。
- Windows：`FlushFileBuffers`、Windows file handle、`MoveFileEx` / `ReplaceFile`、Windows long path、UTF-8 path、disk full、permission denied、partial write、staging cleanup、atomic publish、checksum mismatch、restart index rebuild。
- CTest/CMake：storage 高并发测试可并行；recovery/snapshot/catch-up/crash 类测试低并发，必要时 `CTEST_PARALLEL_LEVEL=1`；本 plan 阶段不修改 CTest filter 或 CMakePresets。

## Phase Plan

### Phase 0: Existing Boundary Review

- 复核 `RaftNode` 默认装配 `MetadataStateMachine`，metadata 写请求通过 `ProposeMetadata()` 进入 Raft 提交链路。
- 复核 `MetadataService` bucket/object lifecycle RPC 现状和 `MetadataStateMachine` PENDING/COMMITTED/DELETED 语义。
- 复核 no-KV 禁止项，保留 `tests/no_kv_surface_audit.cmake` 作为后续实现的持续审计入口。
- 复核当前无真实 `StorageNode` / `PlacementManager` / `RepairManager` / `RebalanceManager` / `ScrubManager` 残留。
- 确认新增模块建议路径为 `modules/raft/storage_node`，StorageNode data-plane 不进入 `modules/raft/storage` 的 Raft persistence 语义。
- 确认不应修改的模块：Raft election/replication/apply/snapshot/recovery 主链路、旧 006 文档、CMake/proto/tests。
- 确认可最小对接的模块：metadata manifest 查询、未来 upload/read/delete coordinator、StorageNode registry/heartbeat 只以明确契约进入 control-plane。

### Phase 1: LocalDiskChunkStore MVP

- 先规划本地 chunk 可靠落盘，不先做复杂网络、Repair 或 Rebalance。
- API 覆盖 `WriteChunk`、`ReadChunk`、`DeleteChunk`、`StatChunk`、`ListChunks`。
- 设计 `ChunkIndex`、`ChunkMetadata`、`ChunkState`：live、staging、deleting、deleted、quarantined、corrupted、missing。
- `chunk_id` MVP 使用 `object_id + version + chunk_index`，避免全局去重和内容寻址引入过早复杂度。
- 写入流程：创建 staging 文件 -> 流式写入 -> 计算 checksum -> 校验 expected checksum/size -> flush data -> atomic publish final chunk -> sync parent directory -> 更新 ChunkIndex。
- 读取流程：查 ChunkIndex -> 打开 final chunk -> 校验 checksum -> 返回数据或 checksum mismatch/corrupted。
- 删除流程：支持 chunk tombstone / delayed physical deletion；重复 delete 对 missing/deleted 返回幂等成功。
- 重启恢复：扫描 final/staging/quarantine/deleting 目录，重建 ChunkIndex，清理 stale staging，识别 partial write，损坏 chunk 进入 quarantine。
- 错误分类：disk full、permission denied、IO error、checksum mismatch、conflict、timeout、cancelled、overloaded。

### Phase 2: Cross-Platform Durable File Abstraction

- 为 StorageNode 数据面规划独立 durable file 能力，不用 Raft log/snapshot 持久化语义直接代替 chunk durability。
- Linux：文件写入后使用 `fdatasync` 或 `fsync`，publish 后对父目录执行 directory sync；所有 required operation 失败必须返回明确错误。
- Windows：使用 Windows file handle 和 `FlushFileBuffers`，publish 规划 `MoveFileEx` / `ReplaceFile` 风险边界，确保文件 handle 关闭、replace 语义、目录可见性和错误分类明确。
- 规划 path normalization、UTF-8 path、Windows long path、reserved names、case sensitivity、permission denied、disk full、partial write、atomic publish failure。
- 对无法等价实现的目录 sync 或 atomic replace 语义，不允许静默成功；必须在 durability contract 中记录较弱保证或返回 explicit unsupported/error。

### Phase 3: StorageNodeService / StorageNodeClient

- 规划独立 `StorageNodeService` / `StorageNodeClient`，不混入 `MetadataService` 或 `RaftService`。
- API 至少覆盖：`WriteChunk`、`ReadChunk`、`DeleteChunk`、`StatChunk`、`ListChunks`、`BatchDeleteChunks`、`ScrubChunk`、`RepairChunk`、`ReportHealth`、`ReportCapacity`、`ReportLoad`、`RegisterStorageNode`、`UpdateStorageNodeHeartbeat`。
- 每个 API 必须定义 request/response 字段、错误码、幂等、timeout、retry、cancellation、checksum 和并发语义。
- `StorageNodeClient` 负责 deadline、retry budget、cancellation token、retryable/non-retryable 分类、节点不可用降级、读副本 fallback。
- `StorageNodeService` 负责 admission control、bounded queues、per-chunk lock、local store 调用、structured result，不决定 object commit。

### Phase 4: Minimal Upload / Read / Delete Loop

上传路径：

- `CreateObject` 创建 pending metadata。
- coordinator/client 将对象切分为 chunk。
- 为每个 chunk 调用 Placement 选择 `replica_nodes`。
- 并发 `WriteChunk` 到 StorageNode。
- StorageNode durable 后返回 checksum、size、node_id、durability status。
- 每个 chunk 满足最小成功副本数。
- `CommitObject` 写入 `ChunkRef` manifest。
- 对象进入 COMMITTED 后对 `HeadObject` / `ListObjects` / read 可见。
- 失败时 `AbortObject` 或 pending timeout cleanup；已写成功但未 commit 的 chunk 进入 metadata-driven GC。

读取路径：

- 先查 metadata，不访问未 committed 或 deleted 对象的数据。
- 读取 `ObjectRecord.chunks`，按 offset 排序。
- 按 `replica_nodes`、health、load、corruption facts 选择读副本。
- `ReadChunk` 并执行 checksum on read。
- 副本失败时 fallback 到其他健康副本。
- checksum mismatch 标记 corrupted，触发 scrub/repair 事实，不返回损坏数据。

删除路径：

- `DeleteObject` 先提交 metadata tombstone/DELETED。
- 读路径立即不可见。
- 后台 async delete chunk，支持 `BatchDeleteChunks`。
- 支持 chunk tombstone、delayed physical deletion、delete retry、delete idempotency。
- GC 必须 metadata-driven，不得误删 committed live chunk。

### Phase 5: High Concurrency And Backpressure

- 设计 bounded thread pool、bounded executor、bounded request queue、bounded IO queue、worker pool。
- 所有 `WriteChunk` / `ReadChunk` / `DeleteChunk` / `ScrubChunk` / `RepairChunk` 都有 backpressure、timeout、cancellation 和 rate limit。
- 读路径支持 parallel reads，写路径 controlled writes，删除与 GC 使用独立低优先级队列。
- 使用 per-chunk lock 避免同一 chunk write/delete/read 的冲突。
- `ChunkIndex` 使用 sharded map、lock striping、read-write lock，降低高并发读写锁竞争。
- resource isolation：前台读写与后台 GC/repair/rebalance 分池或分队列，避免后台任务饿死前台请求。

### Phase 6: StorageNode Heartbeat / Registry

- heartbeat 字段至少包括：`node_id`、`capacity`、`used`、`available`、`chunk_count`、`health`、`disk_pressure`、`io_error_count`、`load`、`last_seen`、`node_liveness`。
- StorageNode heartbeat 独立于 Raft heartbeat，不参与 Raft leader election。
- registry 记录 StorageNode endpoint、zone/rack/failure-domain placeholder、capabilities、last_seen、drain/maintenance 状态。
- Placement、read replica selection、Repair、Rebalance 均消费 registry facts。

### Phase 7: PlacementManager / ReplicaPolicy

- 默认策略：3 副本、最小 2 个成功写副本，后续支持配置化。
- 暂不引入 erasure coding。
- 规划 replica count、minimum successful replicas、write success condition、read replica selection、failed replica handling、under-replicated detection。
- Placement 必须 capacity-aware、health-aware、load-aware、disk-pressure-aware，预留 failure-domain placeholder 和 hotspot avoidance。
- metadata commit 必须发生在满足 data durable 条件之后。
- failed replica handling 记录失败事实，避免短时间内反复选择同一 unhealthy/corrupted node。

### Phase 8: GarbageCollector

- 覆盖 pending object timeout、failed upload cleanup、client disconnect cleanup、AbortObject cleanup、orphan chunk GC。
- GC 安全边界以 metadata 为准：只清理 tombstone/DELETED、aborted、timeout pending 或 metadata 确认不再引用的 chunk。
- 支持 background async delete、delayed physical deletion、chunk tombstone、delete retry、delete idempotency。
- committed live manifest 保护必须优先于本地 orphan scan 结果。
- GC 需要持久化或可恢复任务事实，StorageNode 重启后可继续或安全重试。

### Phase 9: ScrubManager / RepairManager

- ScrubManager 执行 background checksum validation，发现 corrupted replica、lost replica、checksum mismatch。
- RepairManager 检测 under-replicated chunk，选择 healthy source replica 和 healthy target node。
- copy chunk between StorageNodes 后，必须先让新副本 durable，再更新 metadata manifest 或 replica health facts。
- repair task 必须 retryable、idempotent、observable，并记录 progress、attempt、last_error。
- corrupted replica 不能作为 repair source；repair 不能让对象读到半修复 manifest。

### Phase 10: RebalanceManager

- 覆盖 capacity rebalance、hotspot rebalance、new node join。
- 后台 chunk migration 使用 source replica -> target replica -> target durable -> metadata coordination/update -> source optional cleanup。
- 避免 half-migrated manifest：manifest 更新必须在目标副本 durable 后进行，源副本删除必须在 metadata 不再需要它之后。
- rebalance task 必须 retryable、idempotent、observable，并记录 progress、attempt、last_error。
- Rebalance 与 Repair/GC 并发时需要 per-chunk coordination，避免删除正在迁移的 live replica。

### Phase 11: Windows/Linux Validation Matrix

- Linux 覆盖 fsync/fdatasync、directory sync、partial write、staging cleanup、atomic publish、checksum mismatch、crash recovery、restart index rebuild、高并发 chunk IO。
- Windows 覆盖 FlushFileBuffers、file handle、MoveFileEx/ReplaceFile、Windows long path、UTF-8 path、disk full、permission denied、restart index rebuild。
- 通用矩阵覆盖 disk full、permission denied、partial write、staging cleanup、atomic publish、checksum mismatch、corrupted quarantine、crash recovery、high-concurrency chunk IO。
- CTest filters 和 CMakePresets 在后续任务中规划；本阶段不修改。
- storage 高并发测试可并行；recovery/snapshot/catch-up/crash 类测试低并发，必要时 `CTEST_PARALLEL_LEVEL=1`。

## Key Technical Choices

- `chunk_id` MVP 使用 `object_id + version + chunk_index`。
- checksum 用于 write/read/repair/migration/scrub 校验。
- checksum 第一阶段不做内容寻址，不做全局去重。
- MVP 上传协调者先使用 client 或 integration coordinator，不引入复杂 gateway。
- Repair/Rebalance 不在第一阶段一次性实现。
- 007 不实现完整多版本/generation，只保留扩展点。
- StorageNode heartbeat 独立于 Raft heartbeat。
- GC 必须 metadata-driven。

## Test Plan

本阶段只规划测试，不实现测试。

- LocalDiskChunkStore 单元测试：write/read/delete/stat/list、目录布局、ChunkIndex 更新。
- checksum on write/read：正确 checksum 成功，错误 checksum 返回 mismatch 并标记 corrupted。
- idempotent write/delete：相同 chunk_id + 相同 checksum/size 重复成功，不同内容返回 conflict；重复 delete 成功。
- ChunkIndex rebuild：重启扫描 final chunk 恢复 live index。
- stale staging cleanup：旧 staging 清理，不进入 live index。
- corrupted quarantine：损坏文件扫描或读取时隔离。
- `CreateObject -> WriteChunk -> CommitObject` 集成测试：commit 前不可见，durable 后 commit 可见。
- ReadObject by manifest：按 offset 排序读取，副本失败 fallback。
- `DeleteObject -> async chunk cleanup`：metadata tombstone 后立即不可见，后台删除 chunk。
- `failed upload -> AbortObject -> GC`：部分写成功不 commit，GC 清理 orphan。
- concurrent `WriteChunk` / `ReadChunk` / `DeleteChunk`：per-chunk lock、sharded index、无数据竞争。
- bounded queue / backpressure：队列满返回 overloaded/timeout，不无限增长。
- crash before fsync：重启不暴露 partial chunk。
- crash after fsync before rename：staging 可清理或恢复，不进入 live。
- crash after rename before parent directory sync：按平台 durability contract 验证恢复边界。
- partial write：size/checksum 不匹配返回明确错误。
- checksum mismatch：读拒绝损坏副本并标记 corrupted。
- disk full：返回 disk full 并更新 health/error count。
- permission denied：返回 permission denied，不静默成功。
- Placement policy：3 副本、最小 2 成功、capacity/health/load/disk-pressure aware。
- GC safety：不得删除 committed live manifest 引用的 chunk。
- Repair/Rebalance：under-replicated/corrupted 修复、新节点加入迁移、目标 durable 后再 metadata coordination。
- Windows/Linux cross-platform validation：覆盖 Phase 11 矩阵。
- no-KV audit：继续验证不恢复 KV surface。

## Generated Artifacts

- [research.md](./research.md)
- [data-model.md](./data-model.md)
- [quickstart.md](./quickstart.md)
- [cross-task-risk-notes.md](./cross-task-risk-notes.md)
- [contracts/storage-node-api.md](./contracts/storage-node-api.md)
- [contracts/storage-node.proto.draft](./contracts/storage-node.proto.draft)
- [contracts/cross-platform-durability-contract.md](./contracts/cross-platform-durability-contract.md)
- [contracts/placement-contract.md](./contracts/placement-contract.md)
- [contracts/gc-repair-rebalance-contract.md](./contracts/gc-repair-rebalance-contract.md)

## Out Of Scope For This Stage

- 不生成 `tasks.md`。
- 不写 StorageNode 生产代码。
- 不写测试实现。
- 不修改真实 proto。
- 不修改 CMake、CMakePresets、`test.sh`、`test.ps1`。
- 不修改 `specs/006-remove-kv-metadata-state-machine/`。
- 不自动进入 `/speckit-tasks`。
