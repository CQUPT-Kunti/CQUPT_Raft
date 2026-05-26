# Feature Specification: Storage Node Data Plane

**Feature Branch**: `007-storage-node-data-plane`  
**Created**: 2026-05-25  
**Status**: Draft  
**Input**: User description: "在现有 metadata-only Raft 对象存储项目上新增工业化高并发跨平台 Storage Node 数据面"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - 上传对象并在数据 durable 后提交元数据 (Priority: P1)

作为对象存储客户端，我可以上传一个对象，系统将对象切分为 chunk，写入多个 StorageNode，并在所需副本 durable 后通过 Raft metadata commit 对象，使对象对读请求可见。

**Why this priority**: 这是 data-plane 的最小闭环。没有真实 chunk 写入和 data durable 后 commit，metadata-only control-plane 仍无法成为可用对象存储。

**Independent Test**: 可以独立执行一次对象上传流程，验证对象在 chunk 写入完成前保持不可见，在满足副本写成功条件并提交 metadata 后可被 `HeadObject` 和 `ListObjects` 观察到。

**Acceptance Scenarios**:

1. **Given** bucket 已存在且对象键未被当前 live 对象占用，**When** 客户端发起对象上传，**Then** 系统先创建 metadata pending 对象，再把对象切分为 chunk，并选择符合副本策略的 StorageNode。
2. **Given** chunk 写入仍在进行，**When** 客户端查询该对象，**Then** 对象仍保持未 committed，不对读请求可见。
3. **Given** 每个 chunk 已满足最小写成功副本数并通过 checksum 校验，**When** 系统提交对象 manifest，**Then** metadata 进入 committed，`ChunkRef.replica_nodes` 记录可读副本位置。
4. **Given** 部分 chunk 写入失败或超时，**When** 对象不能满足写成功条件，**Then** 系统不得提交对象，并必须形成可重试或可 abort 的明确结果。

---

### User Story 2 - 读取 committed 对象并校验 chunk (Priority: P2)

作为对象存储客户端，我可以读取一个已 committed 对象，系统先查询 metadata，得到 chunk manifest 和 replica_nodes，再从 StorageNode 读取 chunk，并校验 checksum 后返回数据。

**Why this priority**: 对象读路径必须证明 control-plane 和 data-plane 能协同工作，并且不会绕过 committed-only 可见性规则。

**Independent Test**: 可以预置一个 committed 对象及其 chunk 副本，读取对象时验证系统按 manifest 顺序读取 chunk、校验 checksum，并在副本失败时尝试可用替代副本。

**Acceptance Scenarios**:

1. **Given** 对象处于 committed 且 manifest 完整，**When** 客户端读取对象，**Then** 系统先读取 metadata，再按 chunk offset 组装返回内容。
2. **Given** 首选副本不可用，**When** 读取 chunk 失败，**Then** 系统按读副本选择策略尝试其他健康副本，并记录失败副本事实供后续 repair 使用。
3. **Given** 某个副本 checksum mismatch，**When** 读取该 chunk，**Then** 系统拒绝使用该副本的数据，标记 corrupted replica，并尝试其他副本或返回明确错误。
4. **Given** 对象未 committed、已 aborted 或已 deleted，**When** 客户端读取对象，**Then** 系统不得直接访问 StorageNode 返回旧数据。

---

### User Story 3 - 删除对象并异步清理 chunk (Priority: P3)

作为对象存储客户端，我可以删除对象，系统先通过 metadata tombstone/DELETED 状态保证删除语义，再由后台 GC 异步清理 StorageNode 上的物理 chunk。

**Why this priority**: 删除语义必须由 metadata source of truth 先行建立，物理删除可以异步完成，但不能让旧 chunk 数据重新变成 live 对象。

**Independent Test**: 可以删除一个 committed 对象，立即验证外部查询不可见，再等待后台 GC 清理 chunk，并验证重复删除和重启后继续清理都保持幂等。

**Acceptance Scenarios**:

1. **Given** 对象处于 committed，**When** 客户端发起删除，**Then** metadata 先写入 tombstone/DELETED，读路径立即不再返回该对象。
2. **Given** metadata 删除已提交但物理 chunk 尚未删除，**When** 后台 GC 执行，**Then** 它只能清理 tombstone 指向且不再属于 committed live manifest 的 chunk。
3. **Given** 删除请求重复到达，**When** request_id 与原请求一致，**Then** 系统返回幂等结果，不产生重复副作用。
4. **Given** StorageNode 在删除过程中重启，**When** 节点恢复，**Then** 未完成的删除任务可以继续或安全重试。

---

### User Story 4 - 查看 StorageNode 容量、健康和负载 (Priority: P4)

作为系统管理员，我可以查看 StorageNode 的容量、健康、负载、chunk_count、磁盘错误和节点存活状态，用于 Placement、Repair 和 Rebalance。

**Why this priority**: 没有 StorageNode 状态上报，副本放置、故障隔离、修复和再均衡都只能盲选节点。

**Independent Test**: 可以启动多个 StorageNode，模拟容量变化、磁盘压力、IO error 和负载变化，验证 control-plane 能看到及时、明确、可用于决策的心跳状态。

**Acceptance Scenarios**:

1. **Given** StorageNode 正常运行，**When** 它上报心跳，**Then** control-plane 能看到 node_id、capacity、used、available、chunk_count、health、disk pressure、IO error count 和 load。
2. **Given** StorageNode 心跳超时，**When** Placement 或读副本选择需要该节点，**Then** 系统不得把它当作健康首选目标。
3. **Given** StorageNode 进入磁盘压力或 IO error 增长状态，**When** 新 chunk 需要放置，**Then** Placement 应降低或禁止选择该节点。

---

### User Story 5 - StorageNode 重启后恢复本地 chunk 状态 (Priority: P5)

作为系统维护者，我可以在 StorageNode 重启后扫描本地 chunk 目录，重建 chunk index，清理 stale staging 文件，识别 partial、corrupted 或 orphan chunk。

**Why this priority**: data-plane 的可靠性不能只依赖内存索引。崩溃、断电和进程重启后，节点必须能从磁盘事实恢复可读 chunk 集合。

**Independent Test**: 可以在 chunk 写入、publish、删除和 staging cleanup 的不同边界注入崩溃，重启后验证索引重建结果、staging 清理结果和 corrupted quarantine 结果。

**Acceptance Scenarios**:

1. **Given** StorageNode 在 staging 写入完成前崩溃，**When** 它重启扫描，**Then** incomplete staging 不得出现在 live chunk index 中。
2. **Given** StorageNode 在 atomic publish 后崩溃，**When** 它重启扫描，**Then** 已发布且 checksum 正确的 chunk 应被重建到 index。
3. **Given** chunk 文件损坏或 checksum mismatch，**When** 它被扫描或读取，**Then** 系统必须 quarantine 或标记 corrupted，不得把它作为健康副本返回。
4. **Given** 本地存在 metadata 不再引用的 orphan chunk，**When** metadata-driven GC 确认安全边界，**Then** 系统可以后台清理该 chunk。

---

### User Story 6 - 自动修复和再均衡 chunk 副本 (Priority: P6)

作为系统维护者，我可以在副本丢失、checksum mismatch 或节点容量不均衡时，通过 RepairManager 和 RebalanceManager 自动修复和再均衡 chunk 副本。

**Why this priority**: 工业化对象存储不能只在首次上传成功时正确，还必须能长期维持副本数、健康度和容量分布。

**Independent Test**: 可以模拟节点下线、副本损坏、容量倾斜和热点倾斜，验证系统生成可重试、幂等、可观察的 repair/rebalance 任务，并在完成后更新 manifest 或副本健康状态。

**Acceptance Scenarios**:

1. **Given** 某 chunk 低于目标副本数，**When** RepairManager 检测到 under-replicated 状态，**Then** 它应选择健康源副本和目标节点补齐副本。
2. **Given** 某副本 checksum mismatch，**When** ScrubManager 或读路径发现损坏，**Then** RepairManager 应避免使用损坏副本作为源，并尝试从健康副本恢复。
3. **Given** 新节点加入或容量分布明显不均衡，**When** RebalanceManager 运行，**Then** 它应生成后台迁移任务，并与 metadata manifest 更新协同，避免读到半迁移状态。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 当前项目是现有 CQUPT_Raft 的二次开发，不是新建项目，不是重写 Raft，不是恢复旧 KV demo。
- 当前主路径已经完成 metadata-only 收口：Raft 节点默认装配 `MetadataStateMachine`，并注册 `MetadataService`。
- metadata 写请求已经通过 `RaftNode::ProposeMetadata()` 进入 Raft 提交链路。
- Metadata RPC 已有 bucket/object 生命周期接口：`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject`、`HeadObject`、`ListObjects`，并保留 bucket 级操作。
- 对象状态已有 `PENDING`、`COMMITTED`、`DELETED`，但目前没有独立 `uploading` 或 `deleting` 状态。
- `ObjectRecord` 已记录 bucket、object_key、object_id、version、size、etag、state、chunks、create_time、commit_time 和 delete_time。
- `ChunkRef` 已记录 chunk_id、offset、size、replica_nodes 和 checksum。
- `MetadataStateMachine` 已实现 `CreateObject -> pending`、`CommitObject -> committed`、`AbortObject -> deleted/tombstone`、`DeleteObject -> deleted/tombstone`。
- request_id 幂等已有两层：RaftNode 侧 in-flight/completed proposal 去重，以及 MetadataStateMachine 侧 request_fingerprints_ 持久化去重。
- metadata snapshot V2 已覆盖 bucket、object、object_index、chunk_ref_index、request、tombstone 和 applied boundary。
- Raft 自身的 log/snapshot 持久化已有 checksum、fsync/FlushFileBuffers、staging/rename/publish，但这只是 Raft 元数据持久化，不是 StorageNode chunk 数据落盘。
- CodeGraph 当前索引未发现 `StorageNode`、`Placement`、`RepairManager`、`RebalanceManager` 等数据面符号；`WriteChunk/ReadChunk` 只命中 `ChunkRef` 编解码辅助能力，不代表真实 chunk IO。

### Targeted Gaps Or Risks

- 当前系统已经能记录 chunk manifest 元数据，但没有真实 StorageNode 写入、读取、删除、校验、本地落盘和恢复。
- 当前 `ObjectRecord.chunks + chunk_ref_index_` 已承担 ObjectManifest 元数据角色，但不负责对象切片算法，也不负责真实 chunk 数据写入。
- 当前 checksum 主要用于 metadata 字段和 Raft storage/snapshot 校验，尚未覆盖 chunk 写入、读取、修复、迁移过程中的数据校验。
- 当前删除流程已有 Raft tombstone 和 DELETED 状态，尚未实现后台异步删除 chunk，也没有最终 GC 状态。
- 当前版本控制已有 object_id、version 字段和部分 object_id 冲突检查，尚未实现 generation、多版本并存和完整并发覆盖控制。
- 如果 data-plane 规划不明确，后续实现可能错误地把对象字节写入 Raft log，或把 StorageNode 退化成 KV 存储。
- 如果跨平台 durability 只按 Linux 设计，Windows 上可能出现静默降级或半文件被误认为有效 chunk。

### Non-Goals

- 不重新引入 `CommandType::kSet`。
- 不重新引入 `CommandType::kDelete`。
- 不重新引入 `KvStateMachine`。
- 不重新引入 `KvService`。
- 不重新引入 `raft_kv_client`。
- 不重新引入 `DebugGetValue`。
- 不恢复 KV proto。
- 不恢复 KV target。
- 不恢复 KV fallback。
- 不恢复 KV regression-only path。
- tests 中不得使用 SetCommand、DeleteCommand 或 KV 状态机断言。
- 不把对象数据写入 Raft log。
- 不把 StorageNode 设计成 KV 存储。
- 不用 KV 替代 chunk/object 数据面。
- 不重写 Raft 选举、复制、commit/apply、snapshot、restart recovery 或 follower catch-up 核心语义。
- 不让 StorageNode 决定对象是否 committed。
- 不让 MetadataStateMachine 保存 chunk 字节。
- 不在 specify 阶段修改生产代码、CMake、proto 或测试实现。

### Platform Scope

- 007 必须从一开始支持 Windows/Linux 双平台设计，不能只写 Linux-only 路径。
- 路径、文件、目录、atomic publish、directory sync、checksum、staging cleanup 和 restart recovery 必须有跨平台语义。
- Linux validation 必须覆盖 fsync/fdatasync、directory sync、partial write、staging cleanup、atomic publish、checksum mismatch、crash recovery 和高并发 chunk IO。
- Windows validation 必须覆盖 FlushFileBuffers、Windows file handle、MoveFileEx/ReplaceFile 语义、Windows long path、UTF-8 path、permission error、disk full 和 restart index rebuild。
- CMakePresets、CTest 和跨平台测试矩阵必须在后续 plan/tasks 中规划，但 specify 阶段不修改构建文件。

### Edge Cases

- 当 chunk 写入只完成部分副本时，对象不得被 committed，且已写成功的孤儿 chunk 必须能被 abort/GC 清理。
- 当同一 chunk_id 被重复写入且内容与 checksum 完全一致时，写入应幂等；当内容或 checksum 不一致时，必须返回冲突。
- 当客户端在上传过程中断开，系统必须能通过 pending timeout、AbortObject 或后台 GC 清理已写入但未 committed 的 chunk。
- 当 StorageNode 在 staging 写入、checksum 计算、fsync、rename 或父目录同步任一阶段崩溃，重启后不得出现半有效 chunk。
- 当磁盘满、权限错误或 IO error 发生时，StorageNode 必须返回明确错误并更新健康状态，不得静默降级。
- 当 committed 对象删除后物理 chunk 尚未清理，读路径仍必须以 metadata tombstone/DELETED 为准。
- 当 GC 扫描发现本地 orphan chunk，必须先确认 metadata 安全边界，不得误删 committed live chunk。
- 当副本 checksum mismatch 或节点失联，读路径、ScrubManager 和 RepairManager 必须避免把损坏副本当作健康源。
- 当 Rebalance 迁移过程中发生读、删除或再次迁移，系统必须避免 manifest 半更新导致不可读或读到旧删除对象。
- 当 StorageNode heartbeat 过期，Placement 和读副本选择必须把它视为不健康或降权。
- 当大量并发读写删除发生，系统必须有 bounded executor、队列、backpressure、超时和取消控制，避免无界内存或磁盘压力。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST add a StorageNode data-plane concept while preserving the existing Raft metadata control-plane as the source of truth for bucket/object lifecycle, object_id, version, object state, chunk manifest, replica_nodes, request_id idempotency, tombstone, metadata snapshot/recovery, and commit state.
- **FR-002**: System MUST ensure Raft never stores real object payload bytes and Raft log never contains object data content.
- **FR-003**: MetadataStateMachine MUST continue to store metadata only and MUST NOT store chunk bytes.
- **FR-004**: StorageNode MUST be responsible for real chunk byte durability, readability, deletion, checksum verification, local index recovery, health reporting, repair participation, rebalance participation, and safe cleanup.
- **FR-005**: StorageNode MUST NOT decide whether an object is committed; object visibility MUST be decided by metadata state only.
- **FR-006**: Upload flow MUST follow metadata pending -> data chunk durable -> metadata commit.
- **FR-007**: Delete flow MUST follow metadata tombstone/DELETED -> background async chunk cleanup.
- **FR-008**: Read flow MUST query metadata first, then read StorageNode replicas according to ChunkRef and replica_nodes.
- **FR-009**: 007 MUST preserve 006 no-KV收口成果 and MUST NOT reintroduce any KV artifact, fallback, target, proto, regression path, or assertion style.
- **FR-010**: System MUST define StorageNodeService capabilities for WriteChunk, ReadChunk, DeleteChunk, StatChunk, ListChunks, BatchDeleteChunks, ScrubChunk, RepairChunk, ReportHealth, ReportCapacity, ReportLoad, RegisterStorageNode, and UpdateStorageNodeHeartbeat.
- **FR-011**: System MUST define StorageNodeClient capabilities for accessing StorageNode operations with timeout, retry, cancellation, and explicit error classification.
- **FR-012**: System MUST define ChunkStore and LocalDiskChunkStore responsibilities for durable write, atomic publish, checksum, read verify, delete, quarantine, and restart recovery.
- **FR-013**: System MUST define ChunkIndex responsibilities for local chunk lookup, stat, list, startup rebuild, concurrent updates, sharding or lock striping, and consistency with disk state.
- **FR-014**: System MUST define StorageNodeHeartbeat fields including node_id, capacity, used, available, chunk_count, health, disk pressure, IO error count, and load.
- **FR-015**: System MUST define PlacementManager responsibilities for selecting replica nodes using capacity, health, load, replica count, disk pressure, hotspot signals, and future failure-domain fields.
- **FR-016**: System MUST define ReplicaPolicy responsibilities for replica count, minimum write success replica count, read replica selection, failed replica handling, and under-replicated chunk identification.
- **FR-017**: System MUST define GarbageCollector responsibilities for failed upload cleanup, AbortObject cleanup, pending timeout cleanup, orphan chunk cleanup, background tombstone delete, delayed physical deletion, and safe metadata-driven GC boundaries.
- **FR-018**: System MUST define RepairManager responsibilities for lost replica detection, corrupted replica detection, checksum mismatch repair, automatic replica replenishment, retry, idempotency, and progress reporting.
- **FR-019**: System MUST define RebalanceManager responsibilities for capacity rebalance, hotspot rebalance, background chunk migration, retry, idempotency, progress reporting, and metadata manifest coordination.
- **FR-020**: System MUST define ScrubManager responsibilities for background checksum validation, corrupted chunk marking, and repair triggering.
- **FR-021**: System MUST allow WriteChunk to be idempotent for the same chunk_id, same content identity, and same checksum, while returning explicit conflict for mismatched content identity.
- **FR-022**: System MUST allow DeleteChunk and BatchDeleteChunks to be idempotent so repeated cleanup attempts do not produce inconsistent state.
- **FR-023**: System MUST allow StatChunk and ListChunks to distinguish live, staging, quarantined, deleting, missing, and corrupted local chunk facts.
- **FR-024**: System MUST support concurrent WriteChunk, ReadChunk, DeleteChunk, StatChunk, ListChunks, BatchDeleteChunks, ScrubChunk, and RepairChunk without unbounded request growth.
- **FR-025**: System MUST use bounded thread pool, bounded executor, bounded request queue, bounded IO queue, worker pool, backpressure, timeout control, cancellation control, rate limiting, and resource isolation for StorageNode operations.
- **FR-026**: System MUST support concurrent reads with read-priority behavior where safe, while applying controlled write concurrency to avoid exhausting disk bandwidth or capacity.
- **FR-027**: System MUST support per-chunk lock, sharded ChunkIndex, lock striping, read-write lock, and low-lock-contention behavior as explicit design requirements for future implementation.
- **FR-028**: System MUST support streaming upload, streaming download, batch deletion, and large-object multipart/chunked upload as data-plane capabilities to be planned before implementation.
- **FR-029**: System MUST define chunk-based local disk storage that writes to staging first, computes checksum before publish, fsyncs or flushes data before rename, renames as the atomic publish boundary, and syncs the parent directory after publish.
- **FR-030**: System MUST define checksum-on-write, checksum-on-read, checksum during repair, and checksum during migration.
- **FR-031**: System MUST define partial write detection, stale staging cleanup, corrupted chunk quarantine, restart recovery, chunk index rebuild, disk full handling, permission error handling, IO error propagation, and safe cleanup.
- **FR-032**: System MUST define Linux file durability expectations using fsync/fdatasync and directory sync.
- **FR-033**: System MUST define Windows file durability expectations using FlushFileBuffers, Windows file handles, and MoveFileEx/ReplaceFile-style publish semantics.
- **FR-034**: System MUST define portable filesystem abstraction, durable file abstraction, atomic rename abstraction, directory sync abstraction, path normalization, UTF-8 path handling, and Windows long path handling.
- **FR-035**: System MUST treat StorageNode heartbeat separately from existing Raft heartbeat; Raft heartbeat MUST NOT be considered sufficient StorageNode capacity or health reporting.
- **FR-036**: System MUST ensure replica_nodes in existing ChunkRef remains the metadata carrier for chunk replica placement unless a later protocol plan explicitly extends it.
- **FR-037**: System MUST ensure StorageNode writes succeed before CommitObject makes the object visible.
- **FR-038**: System MUST ensure Placement avoids unhealthy, overloaded, disk-pressure, insufficient-capacity, and recently failed nodes whenever alternatives exist.
- **FR-039**: System MUST ensure read replica selection can skip unhealthy, corrupted, overloaded, or stale replicas and can report when no healthy replica remains.
- **FR-040**: System MUST identify under-replicated chunks when actual healthy replicas fall below ReplicaPolicy requirements.
- **FR-041**: System MUST ensure pending object timeout and client disconnect cleanup can trigger AbortObject or equivalent metadata-safe cleanup without committing partial uploads.
- **FR-042**: System MUST ensure orphan chunk GC is metadata-driven and MUST NOT delete chunks referenced by any committed live manifest.
- **FR-043**: System MUST ensure background async delete follows metadata tombstone/DELETED and supports delayed physical deletion and chunk tombstone semantics.
- **FR-044**: System MUST ensure RepairManager can copy chunk replicas from a verified healthy source to a selected healthy target and update metadata or replica health facts only after the new replica is durable.
- **FR-045**: System MUST ensure RebalanceManager migration coordinates with metadata commit/manifest update so reads never observe an unusable half-migrated manifest.
- **FR-046**: System MUST expose health, capacity, load, repair progress, rebalance progress, scrub findings, GC progress, and storage error counts as observable facts for operators.
- **FR-047**: System MUST classify errors into retryable, non-retryable, conflict, timeout, overloaded, not found, checksum mismatch, corrupted, disk full, permission denied, IO error, and node unavailable categories.
- **FR-048**: System MUST keep all object data outside Raft metadata snapshot and Raft log snapshot paths.
- **FR-049**: System MUST plan unit tests, integration tests, pressure tests, concurrency tests, fault injection tests, crash recovery tests, partial write tests, staging cleanup tests, atomic publish tests, checksum mismatch tests, corrupted chunk read tests, delete idempotency tests, restart index rebuild tests, orphan chunk GC tests, placement policy tests, repair manager tests, rebalance manager tests, Windows validation, and Linux validation.
- **FR-050**: System MUST plan storage high-concurrency tests so they can run in parallel where safe, while recovery/snapshot/catch-up class tests remain low-concurrency and may use `CTEST_PARALLEL_LEVEL=1`.

### StorageNode API Concepts

- **WriteChunkRequest / WriteChunkResponse**: 表示写入一个 chunk 的请求和结果，必须表达 chunk identity、expected checksum、size、write idempotency、durability result 和错误分类。
- **ReadChunkRequest / ReadChunkResponse**: 表示读取一个 chunk 的请求和结果，必须表达 chunk identity、range 或完整读取、checksum verification result 和错误分类。
- **DeleteChunkRequest / DeleteChunkResponse**: 表示删除或延迟删除一个 chunk 的请求和结果，必须支持幂等。
- **StatChunkRequest / StatChunkResponse**: 表示查询本地 chunk 状态、大小、checksum、存储状态和错误状态。
- **ListChunksRequest / ListChunksResponse**: 表示分页列举本地 chunk，用于扫描、诊断、GC、repair 和 rebalance。
- **StorageNodeHeartbeatRequest / StorageNodeHeartbeatResponse**: 表示 StorageNode 状态上报和控制面接收结果。
- **ChunkLocation**: 表示 chunk 在某个 StorageNode 上的本地位置和健康状态。
- **ChunkReplica**: 表示某个 chunk 的单副本事实，包括 node、checksum、状态、时间和错误信息。
- **ChunkChecksum**: 表示 chunk 校验值和校验算法身份。
- **ChunkWriteResult**: 表示 chunk 写入结果，包括 durable、already_exists、conflict、checksum_mismatch、overloaded、timeout 或 IO error。

### Key Entities *(include if feature involves data)*

- **StorageNode**: 真实 chunk 数据面的节点，负责本地磁盘 chunk 的 durable write、read verify、delete、index rebuild、health report、repair 和 rebalance participation。
- **StorageNodeService**: 对外暴露 chunk 数据面能力的服务边界，负责接收 chunk 级请求并返回明确结果。
- **StorageNodeClient**: control-plane、coordinator 或客户端访问 StorageNode 的封装边界，负责超时、重试、取消和错误分类。
- **ChunkStore**: 表示 chunk 本地存储能力的抽象边界，负责 staging、checksum、publish、read、delete、quarantine 和 recovery。
- **LocalDiskChunkStore**: 基于本地磁盘的 ChunkStore 形态，必须提供跨平台 durable file 语义。
- **ChunkIndex**: StorageNode 本地 chunk 索引，支持并发查询、更新、删除、扫描重建和状态区分。
- **Chunk**: 对象数据的物理分片，具有 chunk_id、offset、size、checksum、storage state 和 replica identity。
- **ChunkReplica**: 某个 chunk 在单个 StorageNode 上的副本事实，具有健康、checksum、位置、错误和最后验证时间。
- **ObjectManifest**: 逻辑对象的 chunk 列表和副本分布。当前由 `ObjectRecord.chunks + chunk_ref_index_` 承担元数据角色。
- **ReplicaPolicy**: 定义副本数、最小写成功副本数、读副本选择和失败副本处理的策略。
- **PlacementManager**: 基于容量、健康、负载、故障域占位字段、磁盘压力和热点信号选择 chunk 副本位置。
- **StorageNodeHeartbeat**: StorageNode 上报容量、负载、健康、磁盘和错误状态的数据事实。
- **GarbageCollector**: 按 metadata 安全边界清理 failed upload、aborted upload、pending timeout、orphan chunk 和 tombstone chunk 的后台能力。
- **RepairManager**: 检测并修复 lost replica、corrupted replica、checksum mismatch 和 under-replicated chunk 的后台能力。
- **RebalanceManager**: 处理容量再均衡、热点再均衡和后台 chunk 迁移的能力。
- **ScrubManager**: 后台校验 chunk checksum、标记 corrupted chunk 并触发 repair 的能力。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The 007 specification identifies at least six independently testable user stories covering upload, read, delete, StorageNode observability, restart recovery, and repair/rebalance.
- **SC-002**: The 007 specification contains zero supported KV revival paths and explicitly forbids all listed KV artifacts, including CommandType::kSet, CommandType::kDelete, KvStateMachine, KvService, raft_kv_client, DebugGetValue, KV proto, KV target, KV fallback, and KV regression-only paths.
- **SC-003**: A future implementation based on this specification can validate that an object remains invisible before data durability and becomes visible only after metadata commit in 100% of upload acceptance scenarios.
- **SC-004**: A future implementation based on this specification can validate checksum-on-write and checksum-on-read for 100% of committed chunk replicas used to serve reads.
- **SC-005**: A future implementation based on this specification can survive crashes at staging write, checksum, fsync/flush, rename, and parent directory sync boundaries without exposing partial chunk files as valid live chunks.
- **SC-006**: A future implementation based on this specification can classify and report disk full, permission denied, checksum mismatch, corrupted chunk, timeout, overloaded, not found, and node unavailable outcomes without silent success.
- **SC-007**: A future implementation based on this specification can run a cross-platform validation matrix on Windows and Linux for chunk durability, restart index rebuild, checksum mismatch, atomic publish, and stale staging cleanup.
- **SC-008**: A future implementation based on this specification can sustain at least 100 concurrent chunk operations per StorageNode in stress validation while preserving bounded queues, explicit backpressure, and no committed data loss.
- **SC-009**: A future implementation based on this specification can detect under-replicated or corrupted chunk replicas and create observable, retryable, idempotent repair tasks.
- **SC-010**: A future implementation based on this specification can delete committed objects through metadata tombstone first and later physically clean 100% of no-longer-referenced chunks without deleting any committed live chunk.

## Assumptions

- 007 的默认目标是规划完整 StorageNode data-plane 能力；后续 plan/tasks 可以把实现分阶段落地，但不能改变 control-plane/data-plane 边界。
- 默认副本策略可按 3 副本、最小 2 个成功写副本作为后续 plan 的起点；最终值可在配置设计中调整。
- 007 不引入 erasure coding，优先规划多副本 chunk 数据面。
- 007 不新增完整多版本对象语义；object_id、version 和后续 generation 设计在 plan 阶段评估，但不得破坏已有 metadata commit 可见性。
- 007 不要求把上传协调器定义成单独产品角色；客户端、metadata coordinator 或未来 gateway 都可以作为发起 chunk 写入与 metadata commit 的调用方。
- 007 的安全、鉴权、多租户、配额和加密不是本阶段核心范围，除非后续 plan 明确加入。
- CodeGroup 外部记忆库未在当前工具列表中以独立工具暴露；本 specify 阶段使用 CodeGraph MCP、现有 006 spec/plan 和目标模块规则完成现状确认。
