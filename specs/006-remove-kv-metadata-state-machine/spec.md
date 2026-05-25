# Feature Specification: Remove KV Metadata State Machine

**Feature Branch**: `007-remove-kv-metadata-state-machine`  
**Created**: 2026-05-20  
**Status**: Draft  
**Input**: User description: "彻底删除 KV demo 业务模型及其兼容路径，将系统上层唯一业务模型重构为基于 Raft 的强一致对象存储元数据状态机，并保留现有 Raft 一致性、持久化、快照、追赶、恢复与跨平台验证能力。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - 让系统只保留元数据主路径 (Priority: P1)

作为项目维护者，我需要把当前工程从“Raft KV demo”彻底重定位为“Raft 元数据管理层”，使主构建、主运行路径和主验证路径都不再依赖 KV 业务模型，从而避免系统定位混乱和错误回退到旧语义。

**Why this priority**: 这是本特性的根目标。如果 KV 仍以 demo、兼容模式或回归专用路径存在，系统就仍然不是明确的 metadata-only 产品。

**Independent Test**: 可以独立检查系统支持的业务入口、服务入口、客户端入口和验证入口，确认仅保留 `MetadataCommand`、`MetadataStateMachine`、`MetadataService` 和 `raft_metadata_client`，并确认不再存在任何受支持的 KV 业务路径。

**Acceptance Scenarios**:

1. **Given** 工程完成本特性后，**When** 维护者检查受支持的业务模型入口，**Then** `KVCommand`、`KVStateMachine`、`KVService`、`raft_kv_client` 以及 KV 专用 proto/API/CMake/test 入口不再属于受支持主路径。
2. **Given** 需要对外说明当前系统定位，**When** 维护者描述该工程的业务层职责，**Then** 系统被定义为强一致对象存储元数据管理层，而不是分布式 KV、数据库或兼容 KV 的双模系统。
3. **Given** 主线验证需要运行，**When** 执行受支持的回归验证路径，**Then** 所有仍保留的高价值 Raft 回归场景都通过元数据状态机或其重设计后的元数据验证路径完成，而不是通过 KV 状态机完成。

---

### User Story 2 - 管理对象元数据生命周期 (Priority: P2)

作为元数据客户端使用者，我需要通过 bucket 和 object 语义完成创建、提交、撤销、删除、查询和列举，从而让对象元数据成为系统唯一的业务可见状态，并保持提交前不可见、提交后可见、删除后不可见的强一致规则。

**Why this priority**: 删除 KV 之后，系统必须有一个完整且自洽的主业务模型，否则只会得到“移除了旧路径但没有可用主路径”的半成品。

**Independent Test**: 可以只依赖 metadata 客户端完成 `CreateBucket`、`CreateObject`、`CommitObject`、`HeadObject`、`ListObjects`、`AbortObject` 和 `DeleteObject`，并验证可见性规则与冲突结果，不需要任何真实对象数据或 DataNode。

**Acceptance Scenarios**:

1. **Given** bucket 不存在，**When** 客户端发起 `CreateBucket`，**Then** bucket 被系统接受并可作为后续对象元数据操作的命名空间。
2. **Given** bucket 已存在且对象键尚未被当前有效记录占用，**When** 客户端发起 `CreateObject`，**Then** 系统形成 `PENDING` 对象记录，但 `HeadObject` 与 `ListObjects` 不返回该对象。
3. **Given** 某对象处于 `PENDING`，**When** 客户端发起 `CommitObject`，**Then** 该对象进入 `COMMITTED`，并能够被 `HeadObject` 和 `ListObjects` 返回。
4. **Given** 某对象处于 `PENDING` 或 `COMMITTED` 的合法可撤销状态，**When** 客户端发起 `AbortObject` 或 `DeleteObject`，**Then** 该对象在外部查询路径上保持不可见，并且旧请求不能凭借过期上下文让它重新可见。

---

### User Story 3 - 在并发、重试和切主下保持正确结果 (Priority: P3)

作为可靠性验证者，我需要在多客户端并发写入、重复请求、超时重试和 leader 切换场景下仍然得到唯一且一致的元数据结果，从而确保 metadata-only 主路径能够承载工业级控制面负载，而不是只在串行 demo 下工作。

**Why this priority**: 元数据层是未来对象存储控制面的核心。如果并发、背压、幂等和切主语义不成立，删除 KV 只会把旧 demo 替换成新的不稳定 demo。

**Independent Test**: 可以在不引入真实对象数据的前提下，同时对同一 bucket 发起并发 `CreateObject`、`CommitObject`、`HeadObject`、`ListObjects`、`DeleteObject` 和重复 `request_id` 请求，验证顺序 apply、幂等、冲突和显式背压结果。

**Acceptance Scenarios**:

1. **Given** 多个客户端并发提交相同 `request_id` 的同一写意图，**When** 请求因超时、重试或重定向再次到达 leader，**Then** 系统返回同一逻辑结果，不产生重复提交，也不丢失首次已提交结果。
2. **Given** 元数据写入压力超过系统允许的受控范围，**When** 新写请求继续到达，**Then** 系统返回明确的拒绝、背压或超时结果，而不是静默丢弃或无限堆积请求。
3. **Given** 写请求持续提交且查询并发执行，**When** 客户端执行 `HeadObject` 或 `ListObjects`，**Then** 查询只观察到按 Raft 提交顺序形成的已提交状态，不观察到乱序 apply 或半完成状态。
4. **Given** leader 在部分客户端超时后切换，**When** 客户端用原 `request_id` 向新 leader 重试，**Then** 新 leader 返回与已提交事实一致的结果，不重复 apply，也不丢失已提交元数据命令。

---

### User Story 4 - 在恢复和追赶后保持元数据一致 (Priority: P4)

作为回归测试维护者，我需要在 snapshot、restart recovery、state machine replay 和 follower catch-up 后看到与切换前相同的已提交元数据视图，从而确保删除 KV 后，Raft 的高价值可靠性能力仍通过 metadata-only 路径持续得到验证。

**Why this priority**: 本项目的核心价值是 Raft 一致性、持久化、恢复和跨平台验证；替换业务模型不能削弱这些既有能力的可验证性。

**Independent Test**: 可以在高并发元数据写入之后执行 snapshot、重启、日志回放、follower catch-up 和 leader failover，并比对恢复前后的已提交 bucket/object 视图与请求幂等行为。

**Acceptance Scenarios**:

1. **Given** 一组 bucket、object、删除事实和请求去重事实已经提交，**When** 系统执行 snapshot、重启并恢复，**Then** `LoadSnapshot + Replay` 后得到的 `HeadObject`/`ListObjects` 结果与恢复前一致。
2. **Given** follower 在高并发写入期间落后，**When** follower 完成 catch-up，**Then** 它收敛到与 leader 相同的已提交元数据状态，不丢失删除事实，也不重复应用已提交命令。
3. **Given** 回归验证需要覆盖 Raft 的重启、快照和切主场景，**When** 维护者执行受支持的验证矩阵，**Then** 所有这些场景都经由元数据状态机验证，而不是经由 KV 状态机回退验证。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 当前工程已经具备 Raft 核心能力，包括 leader election、log replication、commit/apply、SegmentLogStorage、SnapshotStorage、crash recovery、follower catch-up、restart recovery、state machine replay 以及 CMake/CTest 验证体系。
- 当前工程仍保留 KV 业务模型及其相关状态机、服务、客户端和验证入口，这与系统要演进成对象存储元数据管理层的目标发生冲突。
- 当前项目已经具备 Windows / Linux 跨平台约束；本特性必须在不削弱这些约束的前提下完成业务模型替换。

### Targeted Gaps Or Risks

- KV 与 metadata 业务模型并存会形成错误的产品定位，使维护者和测试路径继续依赖已过时的 KV 语义。
- 只“新增 metadata 主路径但保留 KV fallback”会掩盖缺失的迁移工作，导致主线回归仍然被旧状态机托底。
- 元数据层如果没有明确的并发、幂等、顺序 apply、快照一致性和恢复规则，将无法承载工业级控制面压力。
- 如果没有明确区分“元数据命令历史”和“真实对象数据”，系统可能错误地把对象或 chunk 数据放入共识日志。

### Non-Goals

- 不实现 DataNode。
- 不实现真实 object 或 chunk 数据存储。
- 不实现 `PutChunk`、`GetChunk`、`DeleteChunk`。
- 不实现 BlobStore 或 SegmentBlobStore。
- 不实现纠删码、数据节点副本复制或后台数据修复。
- 不把系统改造成通用分布式 KV、数据库或对象数据面。
- 不以 RocksDB、MySQL、SQLite 或类似数据库替换当前系统定位。
- 不改变已保留的 Raft 核心语义、durability contract 和跨平台约束，除非为承载 metadata-only 业务模型所必需且仍保持同等外部保证。
- 不保留 KV fallback、KV compatibility mode 或 KV regression-only path。

### Platform Scope

- Linux 可以作为高并发、快照、重启和切主演示的主要验证环境，但所有 Linux 专属验证结论都必须标记为 Linux-specific。
- Windows 必须保持与 Linux 等价的 metadata durability、恢复和错误语义；如果某个平台暂时无法提供等价行为，系统必须返回明确错误或在后续工作中显式记录较弱保证，而不能静默降级。

### Edge Cases

- 当 `DeleteBucket` 到达时 bucket 内仍存在 `PENDING`、`COMMITTED` 或尚未被清理完成的对象事实，应如何保持结果确定且不破坏恢复语义？
- 当相同 `request_id` 被并发用于相同意图和不同意图时，系统如何分别处理幂等重放与幂等冲突？
- 当 `CommitObject`、`AbortObject` 和 `DeleteObject` 针对同一对象并发竞争时，系统如何确保只形成一个最终可恢复的结果？
- 当 snapshot 保存期间仍有 apply 和查询并发执行时，系统如何避免外部查询看到撕裂视图？
- 当 leader 切换发生在客户端超时之后、但在客户端决定重试之前，系统如何保证旧请求既不会丢失，也不会重复生效？
- 当 follower 在高并发写入后进行日志追赶或在快照后恢复时，系统如何确保对象删除事实和请求去重事实不被遗漏？
- 当对象已经删除后有合法的新建请求到达，同时旧的创建或提交请求又被重放时，系统如何区分“新对象生命周期”与“过期请求重放”？

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST remove KV business artifacts from the supported product surface, including the KV command model, KV state machine, KV service, KV client, KV-specific business proto/API exposure, KV-only CMake targets, and KV-only validation entry points.
- **FR-002**: System MUST define metadata management as the sole supported business model above the preserved Raft core and MUST NOT retain KV fallback, KV compatibility mode, or KV regression-only paths.
- **FR-003**: System MUST preserve the currently verified Raft core capabilities this project depends on: leader election, log replication, commit/apply, SegmentLogStorage, SnapshotStorage, crash recovery, follower catch-up, restart recovery, state machine replay, and CMake/CTest-based validation.
- **FR-004**: System MUST expose metadata write operations named `CreateBucket`, `DeleteBucket`, `CreateObject`, `CommitObject`, `AbortObject`, and `DeleteObject`.
- **FR-005**: System MUST expose metadata query operations named `HeadObject` and `ListObjects`.
- **FR-006**: System MUST persist replicated business intents as `MetadataCommand` history and MUST NOT use the replicated log as storage for real object data or real chunk payload bytes.
- **FR-007**: System MUST maintain metadata state using `BucketRecord`, `ObjectRecord`, `ChunkRef`, `RequestRecord`, and an optional `TombstoneRecord`.
- **FR-008**: System MUST model object lifecycle with `ObjectState` values `PENDING`, `COMMITTED`, and `DELETED`.
- **FR-009**: System MUST serve `HeadObject` and `ListObjects` from the current `MetadataStateMachine` view rather than by scanning historical command records.
- **FR-010**: System MUST define bucket-scoped object management so object lifecycle, listing, and deletion semantics are evaluated within a named bucket.
- **FR-011**: System MUST reject `DeleteBucket` while the bucket still contains visible objects, in-progress objects, or other active object facts that would make bucket removal inconsistent.
- **FR-012**: System MUST ensure `AbortObject` only terminates an in-progress object lifecycle and keeps that aborted object non-visible to `HeadObject` and `ListObjects`.
- **FR-013**: System MUST allow a later `CreateObject` with a new `request_id` to begin a new object lifecycle after an earlier abort or delete, while still rejecting stale retries that belong to an older lifecycle.
- **FR-014**: System MUST apply committed `MetadataCommand` entries strictly in ascending Raft log index order.
- **FR-015**: System MUST prevent duplicate apply of already committed `MetadataCommand` entries and MUST prevent loss of committed metadata commands across leader switch, replay, restart, or follower catch-up.
- **FR-016**: System MUST ensure only `COMMITTED` objects are externally visible; `PENDING`, aborted, and deleted objects MUST remain non-visible to `HeadObject` and `ListObjects`.
- **FR-017**: System MUST enforce `request_id` idempotency for metadata write operations under concurrent duplicates, timeout retries, leader changes, restart recovery, and replay.
- **FR-018**: System MUST return explicit outcomes for idempotent replay, idempotency conflict, state conflict, not found, not leader, invalid request, overload, and timeout conditions.
- **FR-019**: System MUST support concurrent multi-client `CreateObject`, `CommitObject`, `HeadObject`, `ListObjects`, and `DeleteObject` activity while preserving one coherent committed metadata state.
- **FR-020**: System MUST allow concurrent `HeadObject` and `ListObjects` reads without exposing out-of-order apply results, duplicate committed state, or partially published snapshot state.
- **FR-021**: System MUST provide bounded admission or backpressure behavior for overloaded metadata writes so each request is either explicitly accepted into a bounded path or explicitly rejected or timed out; no metadata write may be silently dropped.
- **FR-022**: System MUST preserve durable deletion facts so stale create or commit retries cannot resurrect objects that have already been deleted in a newer valid lifecycle.
- **FR-023**: System MUST define deterministic conflict handling for duplicate bucket creation, duplicate object creation, commit of missing objects, commit of terminal-state objects, delete of unknown objects, and concurrent commit/delete races.
- **FR-024**: System MUST define snapshot and recovery behavior so `LoadSnapshot + Replay` reconstructs committed buckets, committed objects, required request deduplication facts, and any deletion facts needed to preserve consistency.
- **FR-025**: System MUST ensure snapshot save and publish do not break query/apply consistency: queries may observe either the committed state before the snapshot point or the committed state after it, but never a torn mix.
- **FR-026**: System MUST ensure follower catch-up after concurrent metadata writes converges to the same committed metadata state as the leader.
- **FR-027**: System MUST ensure leader switch after concurrent metadata writes preserves all already committed metadata, does not re-apply committed writes twice, and does not expose uncommitted writes as committed state.
- **FR-028**: System MUST migrate every still-valuable Raft regression scenario away from `KVStateMachine` to `MetadataStateMachine` or to a redesigned metadata-focused validation path.
- **FR-029**: System MUST keep CMake and CTest validation aligned with the metadata-only product surface by removing KV-only supported entry points from the mainline verification path and validating the metadata path instead.
- **FR-030**: Any Linux-specific validation path MUST be explicitly labeled and MUST identify the Windows cross-platform expectation or an explicit equivalent-error behavior.
- **FR-031**: Any change affecting persistence, snapshot, restart recovery, or replication MUST define the expected behavior under crash, partial publish, replay, failover, or catch-up conditions that matter to metadata consistency.
- **FR-032**: System MUST remain a metadata management layer only and MUST NOT require DataNode, `PutChunk`, `GetChunk`, `DeleteChunk`, BlobStore, SegmentBlobStore, erasure coding, data-node replica replication, or database replacement to satisfy this feature.

### Metadata Lifecycle Rules

- `CreateBucket` establishes the bucket namespace but does not create any externally visible object.
- `CreateObject` opens a new object lifecycle in `PENDING`; visibility begins only after `CommitObject`.
- `CommitObject` is the visibility boundary for object metadata and is the point after which `HeadObject` and `ListObjects` may return the object.
- `AbortObject` cancels an in-progress lifecycle and must leave the object externally invisible.
- `DeleteObject` ends the visible lifecycle and preserves a deletion fact strong enough to reject stale retries from older lifecycles.
- `HeadObject` and `ListObjects` read current metadata state, not historical log content.

### Key Entities *(include if feature involves data)*

- **MetadataCommand**: 表示被 Raft 复制和持久化的元数据业务意图，是对象与 bucket 生命周期变化的唯一业务写入历史。
- **MetadataStateMachine**: 表示系统唯一受支持的业务状态机，负责按提交顺序应用元数据命令、维护当前元数据视图并服务查询。
- **BucketRecord**: 表示 bucket 命名空间及其可操作状态，是对象键归属和 bucket 删除前置条件的判断基础。
- **ObjectRecord**: 表示单个对象当前生命周期的元数据事实，包含对象身份、归属 bucket、逻辑 chunk 引用、请求关联和当前状态。
- **ChunkRef**: 表示对象元数据中的逻辑 chunk 引用，只表达元数据关系，不承载真实 chunk 内容。
- **RequestRecord**: 表示写请求幂等和冲突判断所需的请求事实，用于跨重试、切主和恢复保持一致结果。
- **TombstoneRecord**: 表示对象已删除或其他必须保留的删除事实，用于防止旧请求重放导致对象复活。
- **ObjectState**: 表示对象生命周期状态，只允许 `PENDING`、`COMMITTED`、`DELETED` 三种业务状态。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The supported business surface defined by this feature contains exactly four named top-level metadata artifacts: `MetadataCommand`, `MetadataStateMachine`, `MetadataService`, and `raft_metadata_client`, and zero supported KV business artifacts.
- **SC-002**: All six metadata write operations and both metadata query operations have explicit acceptance coverage or explicit functional requirements describing visible behavior.
- **SC-003**: The specification preserves all ten named baseline Raft capabilities without requiring any supported KV dependency: leader election, log replication, commit/apply, SegmentLogStorage, SnapshotStorage, crash recovery, follower catch-up, restart recovery, state machine replay, and build/test validation.
- **SC-004**: The specification defines committed-only visibility, ordered apply, idempotent retry handling, overload handling, snapshot consistency, restart recovery, follower catch-up, and leader-switch behavior for 100% of metadata write operations.
- **SC-005**: Every supported regression or validation path described by this feature uses `MetadataStateMachine` or a metadata-focused redesign; zero supported regression paths rely on `KVStateMachine`.
- **SC-006**: The feature scope explicitly excludes at least nine named data-plane or database non-goals: DataNode, real object data storage, `PutChunk`, `GetChunk`, `DeleteChunk`, BlobStore, SegmentBlobStore, erasure coding or data-node replica replication, and database replacement.

## Assumptions

- 当前项目中已验证的 Raft 核心一致性、持久化、恢复和跨平台约束被视为受保护 baseline，本特性在替换业务模型时必须保留这些外部保证。
- `HeadObject` 与 `ListObjects` 默认针对当前可线性化的已提交元数据视图；本特性不单独引入新的 follower 读模型。
- `DeleteBucket` 的默认规则是 bucket 中不能存在仍然活跃的对象事实，否则删除请求返回确定的冲突结果。
- 被 `AbortObject` 或 `DeleteObject` 终止的旧对象生命周期可以在满足当前状态前置条件时被新的 `CreateObject` 重新开始，但必须使用新的 `request_id`，且旧 `request_id` 只能得到幂等重放或冲突结果。
- `ListObjects` 默认返回 bucket 范围内的确定性 `COMMITTED` 对象集合；分页、前缀过滤、ACL、多租户和配额不属于本特性范围。
- `TombstoneRecord` 在外部模型中是可选命名实体，但删除事实本身是强制语义，不能因为内部表示差异而缺失。
- 本特性只管理 metadata，不保存真实对象内容，不要求真实 chunk 节点、真实数据副本或对象上传下载链路存在。
