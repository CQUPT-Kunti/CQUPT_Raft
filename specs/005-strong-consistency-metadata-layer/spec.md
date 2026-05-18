# Feature Specification: Strong Consistency Metadata Layer

**Feature Branch**: `005-strong-consistency-metadata-layer`  
**Created**: 2026-05-18  
**Status**: Draft  
**Input**: User description: "规划 KV demo 层向基于 Raft 的强一致元数据层演进；Raft 只复制元数据命令，不复制大文件数据；本阶段不实现 StorageNode、真实 chunk 文件存储、大文件真实上传下载、chunk replication、纠删码、rebalance、S3 协议，也不修改 Raft 内核或源码。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - 提交强一致元数据记录 (Priority: P1)

作为 Metadata Client 使用者，我需要创建并提交一个模拟对象元数据记录，使对象在提交前不可见、提交后可通过 Head/List 读取，从而验证元数据层对提交可见性的强一致语义。

**Why this priority**: 这是 KV demo 向强一致元数据层演进的最小可用闭环，直接覆盖 CreateMetadataRecord、CommitMetadataRecord、HeadMetadataRecord 和 ListMetadataRecords 的核心价值。

**Independent Test**: 可以只通过客户端提交一个模拟对象日志，检查 Pending 不可见、Committed 可见、Head/List 返回一致内容，独立证明 metadata commit path 可用。

**Acceptance Scenarios**:

1. **Given** 集群已有可用 leader 且对象键 `object_key` 不存在，**When** 客户端使用唯一 `ClientRequestId` 创建元数据记录，**Then** 系统接受请求并产生 Pending 记录，但 Head/List 不返回该记录。
2. **Given** 同一 Pending 记录已经创建成功，**When** 客户端提交该记录，**Then** 该记录进入 Committed 状态，Head 能返回完整 MetadataRecord，List 能返回该对象键。
3. **Given** 记录已经 Committed，**When** 客户端重复提交相同 `ClientRequestId` 的请求，**Then** 系统返回与首次成功提交等价的结果，不生成重复可见记录。

---

### User Story 2 - 删除与 tombstone 可恢复 (Priority: P2)

作为 Metadata Client 使用者，我需要删除已提交的元数据记录，并确保删除通过 tombstone 表示且可在 snapshot/restart 后恢复，从而验证删除语义不会因重启或日志压缩而回退。

**Why this priority**: 删除语义是元数据层不可缺少的生命周期能力；tombstone 的恢复边界决定后续 StorageNode 与 ChunkStore 能否安全处理历史对象键。

**Independent Test**: 可以在单个对象上完成 create、commit、delete、restart、head/list 验证，独立证明删除状态持久可恢复。

**Acceptance Scenarios**:

1. **Given** 一个对象记录已经 Committed，**When** 客户端执行 DeleteMetadataRecord，**Then** 该对象进入 Deleted tombstone 状态，Head/List 不再返回该对象。
2. **Given** 删除 tombstone 已经被提交，**When** 系统完成 snapshot 并重启，**Then** Head/List 仍不返回该对象，且系统保留删除事实用于拒绝旧请求回放导致的错误复活。
3. **Given** 删除请求使用已成功删除的相同 `ClientRequestId` 重试，**When** 请求再次到达 leader，**Then** 系统返回幂等成功结果且不改变删除状态。

---

### User Story 3 - Leader failover 后验证 committed metadata 不丢失 (Priority: P3)

作为可靠性验证者，我需要在 leader failover 后确认已经 committed 的元数据仍然可见，未提交或 Pending 的记录不会错误对外可见，从而验证强一致元数据层继承 Raft 提交语义。

**Why this priority**: failover 是强一致元数据层面向后续文件系统/对象系统演进的关键可靠性信号，但它依赖 P1/P2 的元数据生命周期已定义。

**Independent Test**: 可以在提交若干记录后切换 leader，再通过 Metadata Client 执行 Head/List 和重复提交验证，独立证明 failover 后的可见性与幂等语义。

**Acceptance Scenarios**:

1. **Given** 多个 MetadataRecord 已经 Committed，**When** 原 leader 不再对外服务且新 leader 产生，**Then** 新 leader 上的 Head/List 能看到相同 committed metadata。
2. **Given** 一个 MetadataRecord 仍处于 Pending，**When** 发生 leader failover，**Then** 客户端无法通过 Head/List 看到该 Pending 记录。
3. **Given** 客户端在 failover 前后用相同 `ClientRequestId` 重试同一提交，**When** 新 leader 处理请求，**Then** 请求结果与已提交事实一致，不能产生重复提交或状态倒退。

---

### User Story 4 - 为 StorageNode 和 ChunkStore 保留扩展边界 (Priority: P4)

作为后续存储层开发者，我需要当前元数据模型显式区分“被 Raft 复制的元数据命令”和“不会被 Raft 复制的大文件数据”，从而可以在未来接入 StorageNode、ChunkStore、切片上传下载和位置汇报而不重写元数据语义。

**Why this priority**: 该能力不直接交付当前 demo 行为，但决定本阶段设计是否能支撑后续真实对象数据路径。

**Independent Test**: 可以通过审查 MetadataRecord 模型、模拟日志字段和 Non-Goals，确认 payload 仅用于模拟/验证，真实 chunk 数据、chunk replication、rebalance、纠删码和 S3 协议均不进入当前范围。

**Acceptance Scenarios**:

1. **Given** 客户端提交包含 chunk manifest 和 mock_locations 的模拟日志，**When** 元数据记录被提交，**Then** Raft 复制的内容只表达元数据和模拟位置，不要求任何真实 chunk 文件存在。
2. **Given** 后续 StorageNode/ChunkStore 需要接入，**When** 规划本 feature 的下一阶段，**Then** 可以沿用 object_key、chunk manifest、checksum、location 引用和提交状态边界，不改变本阶段定义的 committed-only visibility。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 当前 demo 面向 KV 语义：客户端提供 put/get/delete/status/health/metrics 操作，服务端将写请求作为 Raft 命令提交，状态机维护 key/value 视图。
- 当前状态机 snapshot 覆盖 KV 内容，用于重启恢复；本 feature 只规划上层语义演进，不重新分析或修改已有 Raft 内核、持久化、snapshot、catch-up、leader election、RPC status 或 metrics 机制。
- 当前读取语义以 leader 上的可见状态为准；本 feature 将该能力规划为 Metadata Client 对 committed metadata 的 Head/List 验证入口。

### Targeted Gaps Or Risks

- KV 的 key/value 语义不足以表达对象元数据生命周期，缺少 Pending、Committed、Deleted 状态和 commit-only visibility 规则。
- 当前客户端无法模拟对象日志、chunk manifest、上传记录、提交记录、幂等 request_id 或 tombstone 恢复。
- 如果没有明确边界，后续大文件数据路径可能被错误放入 Raft 复制链路，造成共识层承载大 payload、真实 chunk 存储和数据复制职责混淆。
- 删除如果只表现为普通 key erase，snapshot/restart 后无法表达 tombstone 事实，后续可能出现旧请求回放导致对象错误复活。

### Non-Goals

- 不实现 StorageNode。
- 不实现真实 chunk 文件存储。
- 不实现大文件真实上传或下载。
- 不实现 chunk replication。
- 不实现纠删码。
- 不实现 rebalance。
- 不实现 S3 协议。
- 不修改 Raft 内核、Raft 协议语义、持久化格式、snapshot 存储格式、公共 API 行为或源码。
- 不读取、依赖或总结 `NOTREAD.md` 禁止的历史 spec、测试、构建产物、运行数据、第三方依赖、生成文件或其他禁止路径。

### Platform Scope

- 本阶段是 specify 阶段，只定义跨平台一致的元数据语义和验收边界，不定义平台专属实现路径。
- 后续任何涉及 durability、snapshot 或 restart recovery 的实现阶段，都必须延续项目现有 durability contract：required durability operation 不允许 no-op 成功，不同平台必须提供等价行为、明确错误或记录较弱保证。

### Edge Cases

- 重复 `ClientRequestId` 到达时，系统必须返回同一逻辑请求的幂等结果，不产生重复记录、重复提交或重复 tombstone。
- 同一 `object_key` 已存在 Committed 记录时，再次 CreateMetadataRecord 必须明确拒绝或返回等价幂等结果；不得隐式覆盖 committed metadata。
- Pending 记录在 commit 前不得被 Head/List 看到，即使它已经被 Raft 复制或在内部状态中存在。
- DeleteMetadataRecord 对不存在对象、Pending 对象、Committed 对象和已 Deleted 对象必须给出明确、可测试的状态结果。
- Snapshot/restart 后必须恢复 committed metadata 与 tombstone；不得恢复为 Pending 可见状态，也不得丢失删除事实。
- Leader failover 后，已经 committed 的 metadata 不能丢失；未 committed 的请求不能被 Head/List 当作 committed 记录暴露。
- 模拟 payload 过大或字段缺失时，客户端和服务端规划必须提供明确的校验失败语义，避免把大文件真实内容放入 Raft 命令。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST define `StrongConsistencyMetadataStateMachine` as the metadata lifecycle owner for records replicated through Raft, responsible for applying metadata commands, enforcing record state transitions, serving committed-only reads, preserving tombstones, and participating in snapshot/restart recovery.
- **FR-002**: `StrongConsistencyMetadataStateMachine` MUST NOT own StorageNode scheduling, real chunk file IO, large-file upload/download streaming, chunk replication, erasure coding, rebalance, S3 compatibility, or Raft consensus internals.
- **FR-003**: System MUST define `MetadataRecord` as the logical metadata unit identified by `object_key`, with at least `object_size`, `chunk_size`, `chunk_count`, `checksum`, `mock_locations`, `payload`, `request_id`, record state, creation metadata, commit metadata, and deletion metadata.
- **FR-004**: System MUST define `MetadataRecordState` with exactly these externally meaningful lifecycle states: `Pending`, `Committed`, and `Deleted`.
- **FR-005**: System MUST define `ClientRequestId` / `request_id` as the idempotency key for client write intents; retrying the same logical request must return the same logical outcome and must not duplicate records or advance state twice.
- **FR-006**: System MUST define `CreateMetadataRecord` as a write operation that accepts a simulated object metadata log, validates required metadata fields, records a Pending metadata record, and does not make that record externally visible through Head/List.
- **FR-007**: System MUST define `CommitMetadataRecord` as a write operation that transitions a matching Pending record to Committed, records commit metadata, and makes the record visible to Head/List only after the commit is durable through the replicated metadata path.
- **FR-008**: System MUST define `DeleteMetadataRecord` as a write operation that records a Deleted tombstone for a committed object key and removes the object from Head/List visibility without erasing the deletion fact required for recovery and idempotency.
- **FR-009**: System MUST define `HeadMetadataRecord` as a read operation that returns one MetadataRecord only when the latest state for `object_key` is Committed; it must return not-found for Pending, Deleted, never-created, or non-visible records.
- **FR-010**: System MUST define `ListMetadataRecords` as a read operation that returns only Committed records, supports deterministic ordering by object key unless a later stage specifies another order, and excludes Pending records and Deleted tombstones from external visibility.
- **FR-011**: System MUST define the client simulated log format with fields for `request_id`, operation type, `object_key`, `object_size`, `chunk_size`, `chunk_count`, `checksum`, `mock_locations`, `payload`, and optional commit/delete reason metadata.
- **FR-012**: System MUST treat `payload` as simulated metadata payload only; it must not represent or require real large-file bytes, real chunk data, or filesystem-backed object content in this phase.
- **FR-013**: System MUST ensure committed-only visibility: only records in Committed state are visible through HeadMetadataRecord and ListMetadataRecords.
- **FR-014**: System MUST ensure Pending records are not externally visible, including after retry, failover, restart, or snapshot recovery.
- **FR-015**: System MUST ensure Delete uses tombstone semantics: Deleted records are hidden from Head/List but preserved as deletion facts for idempotency, recovery, and old-request conflict handling.
- **FR-016**: System MUST specify snapshot/restart expectations: after recovery, committed metadata and tombstones must be restored; Pending records may be restored only as internal non-visible state if the later implementation explicitly supports it, and must never become visible without CommitMetadataRecord.
- **FR-017**: System MUST specify leader failover expectations: metadata that reached committed state before failover remains visible after a new leader is available; uncommitted or Pending metadata remains non-visible.
- **FR-018**: Metadata Client MUST evolve from KV-oriented commands to metadata-oriented commands that can create simulated object logs, commit records, delete records, head records, list records, and retry requests with a chosen `ClientRequestId`.
- **FR-019**: Metadata Client MUST be able to simulate object logs, chunk manifests, upload records, and commit records without requiring a StorageNode or real chunk file storage.
- **FR-020**: System MUST preserve an explicit extension boundary for future StorageNode/ChunkStore integration: future data-plane components may consume object keys, chunk manifests, checksums and locations, but Raft remains responsible only for metadata commands and metadata state.
- **FR-021**: System MUST define clear conflict outcomes for duplicate object keys, missing Pending records during commit, deletion of unknown objects, deletion of Pending records, and retry after failover.
- **FR-022**: System MUST keep this feature in planning/specification scope only for the current phase; source code, protocol definitions, persistence formats, existing public behavior, tests, and Raft internals are out of modification scope.

### Consistency Semantics

- Linearizability target: successful metadata writes are ordered by the replicated metadata command log, and reads served for Head/List must reflect the committed metadata state visible at the read point.
- Submit visibility: CreateMetadataRecord alone is not sufficient for user-visible existence; CommitMetadataRecord is the visibility boundary.
- Idempotency: `request_id` identifies a logical write intent. The same `request_id` with the same intent is a retry; the same `request_id` with conflicting content must be rejected as an idempotency conflict.
- Deletion: Deleted is a durable tombstone state, not an absence-only erase. Deleted records remain hidden from Head/List but must survive recovery as internal facts.
- Failover: leader change must not expose uncommitted metadata and must not hide metadata that was already committed.

### Client Simulated Log Format

The Metadata Client simulated log MUST be human-readable or machine-readable in a deterministic form and carry these logical fields:

- `request_id`: client-generated idempotency key.
- `operation`: one of create, commit, delete, head, list, or a later explicitly planned metadata-only operation.
- `object_key`: stable object identifier visible to users after commit.
- `object_size`: simulated object byte length, used for metadata validation only.
- `chunk_size`: simulated target chunk size.
- `chunk_count`: number of simulated chunks expected for the object.
- `checksum`: simulated whole-object or manifest checksum.
- `mock_locations`: simulated placement references for chunks or replicas, without requiring real StorageNode existence.
- `payload`: metadata-only payload for verification; not real file content.
- `commit_info`: optional simulated commit metadata such as client timestamp, manifest version, or commit note.
- `delete_info`: optional simulated deletion reason or delete marker metadata.

### Key Entities *(include if feature involves data)*

- **StrongConsistencyMetadataStateMachine**: 元数据生命周期拥有者，应用元数据命令，维护对象键到 MetadataRecord/Tombstone 的状态视图，提供 committed-only Head/List 语义，并定义 snapshot/restart 后的恢复结果。
- **MetadataRecord**: 表示一个对象的强一致元数据记录，包含对象键、大小、chunk manifest 摘要、校验信息、模拟位置、模拟 payload、幂等 request_id 和生命周期状态。
- **MetadataRecordState**: 表示记录生命周期，取值为 Pending、Committed、Deleted；只有 Committed 对外可见。
- **ClientRequestId / request_id**: 客户端写请求幂等键，用于识别重试、冲突和跨 failover/restart 的等价结果。
- **ChunkManifest**: 模拟的 chunk 元数据集合，描述 chunk_size、chunk_count、checksum 和 mock_locations；不包含真实 chunk bytes。
- **Tombstone**: 删除事实，记录对象键已被删除以及删除请求的幂等信息；隐藏于 Head/List，但必须可恢复。
- **Metadata Client**: KV client 的规划后继，负责发起 metadata create/commit/delete/head/list 和模拟日志，不负责真实文件传输。

### Non-Functional Requirements

- **NFR-001**: 元数据操作结果必须可通过用户可见的客户端命令验证，不依赖读取内部 Raft 日志或测试私有状态。
- **NFR-002**: 对象元数据模型必须保持小而明确，避免鼓励把真实大文件 payload 放入复制命令。
- **NFR-003**: 错误结果必须可区分 not leader、invalid argument、not found、idempotency conflict、state conflict 和 internal error 等类别，便于客户端重试或终止。
- **NFR-004**: 规划必须保持上层演进与 Raft 内核解耦，后续实现不得为了 metadata 语义修改已有共识安全性假设。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 评审者能够从 spec 中明确列出 5 个元数据操作：CreateMetadataRecord、CommitMetadataRecord、DeleteMetadataRecord、HeadMetadataRecord、ListMetadataRecords。
- **SC-002**: 评审者能够从 spec 中验证 3 个状态：Pending、Committed、Deleted，并确认只有 Committed 会被 Head/List 返回。
- **SC-003**: 至少 6 类关键场景具备可测试验收描述：提交可见性、Pending 不可见、幂等重试、tombstone 删除、snapshot/restart 恢复、leader failover。
- **SC-004**: 当前阶段的 8 个非目标均被明确排除：StorageNode、真实 chunk 文件存储、大文件真实上传下载、chunk replication、纠删码、rebalance、S3 协议、Raft 内核/源码修改。
- **SC-005**: 元数据模拟日志格式至少包含 9 个指定字段：object_key、object_size、chunk_size、chunk_count、checksum、mock_locations、payload、request_id、operation。
- **SC-006**: 后续 StorageNode/ChunkStore 扩展边界可被独立识别，且不要求重定义 committed-only visibility 或 tombstone 语义。

## Assumptions

- 本阶段只产出 specification，不进入 plan、tasks 或 implementation。
- 当前 KV demo、Raft service、state_machine、client 和 proto 均视为现有 baseline；本阶段不修改其源码、协议或持久化格式。
- Metadata Client 面向验证和演示强一致元数据语义，不面向真实用户文件传输。
- 模拟 `mock_locations` 可以使用字符串形式的位置引用，不要求对应真实节点、磁盘路径或网络端点存在。
- `object_key` 在元数据层内作为用户可见唯一对象标识；同一对象键的覆盖、版本化、多版本读取不属于本阶段。
- ListMetadataRecords 默认返回全部 committed records 的确定性列表；分页、前缀过滤和权限控制留待后续阶段。
- 安全认证、授权、多租户、配额和审计不是本阶段目标，除非后续 feature 单独规划。
