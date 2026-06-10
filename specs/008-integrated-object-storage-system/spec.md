# Feature Specification: Integrated Object Storage System

**Feature Branch**: `008-integrated-object-storage-system`  
**Created**: 2026-06-05  
**Status**: Draft  
**Input**: User description: "008-integrated-object-storage-system: connect the existing Raft metadata control-plane and StorageNode data-plane into an end-to-end distributed object storage prototype with ViewNode service discovery, configurable cluster sizing, persistent node identity, real file upload/download, checksum validation, quorum safety, cross-platform startup, and industrialization boundaries."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - 上传并下载真实对象 (Priority: P1)

作为存储客户端使用者，我希望上传一个真实文件并随后下载同一个对象，系统必须通过元数据一致层生成写入计划、通过数据面保存真实 chunk，并在下载后证明文件内容与上传前完全一致。

**Why this priority**: 这是本阶段最小可演示价值；如果不能端到端保存和取回真实文件，Raft control-plane 与 StorageNode data-plane 仍然只是两个孤立能力。

**Independent Test**: 启动一个配置驱动的小集群，上传一个真实文件，下载到新路径，比较上传前后 SHA-256，且确认真实 payload 没有进入 Raft 日志或快照。

**Acceptance Scenarios**:

1. **Given** ViewNode、Raft MetadataNode 和 StorageNode 均已按配置启动且健康，**When** Client 上传真实文件并提交对象，**Then** 对象状态从 PENDING 变为 COMMITTED，下载文件的 SHA-256 与原始文件完全一致。
2. **Given** 对象写入计划已创建但 chunk 尚未全部写入，**When** Client 查询或下载该对象，**Then** 系统不得向用户暴露未提交对象。
3. **Given** Client 已从 manifest 获取 chunk 位置信息，**When** 某个 chunk 读取返回 checksum 不匹配，**Then** 下载必须失败并报告校验错误，不得静默拼接损坏数据。

---

### User Story 2 - 配置驱动启动完整集群 (Priority: P1)

作为运维或开发者，我希望通过统一配置描述 ViewNode、Raft MetadataNode、StorageNode 的数量、地址、端口、路径和容量，使节点数量不依赖代码常量，也不要求手工维护固定节点名称。

**Why this priority**: 本阶段目标不是固定节点 demo，而是可扩展的系统雏形；配置驱动是后续扩容、测试矩阵和跨平台启动的前提。

**Independent Test**: 使用同一套配置格式分别生成 1/3/5 个 MetadataNode、多个 StorageNode 和可变数量 ViewNode 的启动配置，确认 app 启动时读取配置并按配置注册、发现和暴露状态。

**Acceptance Scenarios**:

1. **Given** 配置声明 3 个 Raft MetadataNode、1 个 ViewNode、3 个 StorageNode，**When** 集群启动，**Then** 所有节点使用配置中的地址、端口、data_dir 和容量信息，客户端不需要硬编码节点端口。
2. **Given** 配置声明 5 个 Raft MetadataNode，**When** 集群启动并完成 leader election，**Then** commit quorum 与 election quorum 按 5 个 voter 计算为 3。
3. **Given** 新增 StorageNode 配置并启动该节点，**When** 节点注册并持续心跳，**Then** 后续写入计划可以选择该健康 StorageNode，且不需要修改代码。

---

### User Story 3 - 服务发现与节点状态观测 (Priority: P2)

作为客户端或管理者，我希望 ViewNode 提供节点注册、服务发现和健康状态观测，使 Client 能先找到可用 MetadataNode，再按 manifest 找到 StorageNode，同时能看到节点容量、心跳和存活状态。

**Why this priority**: 真实系统需要从“知道固定端口”过渡到“发现可用服务”；但 ViewNode 不能越权成为对象元数据或 Raft membership 的一致性权威。

**Independent Test**: 启动 ViewNode 与多个节点，确认 MetadataNode 和 StorageNode 注册后可被查询；停止某个 StorageNode 后，ViewNode 在心跳超时后将其标记为不可用，新的 placement 不再选择它。

**Acceptance Scenarios**:

1. **Given** MetadataNode 和 StorageNode 已启动，**When** 它们向 ViewNode 注册并上报心跳，**Then** ViewNode 展示 node_id、节点类型、地址、端口、容量、健康状态和最后心跳时间。
2. **Given** Client 没有任何 MetadataNode 硬编码地址，**When** Client 请求上传或下载对象，**Then** Client 能通过 ViewNode 找到可用 MetadataNode 地址。
3. **Given** 一个 Raft 新节点注册到 ViewNode，**When** 管理者查看节点状态，**Then** ViewNode 只能显示 REGISTERED、JOINING、LEARNER、VOTER 或 DOWN 等观测状态，不得将注册解释为已加入 Raft membership。

---

### User Story 4 - 节点身份自动分配并持久化 (Priority: P2)

作为节点运行者，我希望系统为节点分配稳定 node_id，并把身份持久化在节点本地目录中，使节点重启后保持同一身份，避免 metadata 中记录的 chunk 副本位置失效。

**Why this priority**: StorageNode 的 chunk 位置依赖稳定 node_id；Raft MetadataNode 的 raft_id 更敏感，也必须明确由配置生成或受控流程管理。

**Independent Test**: 首次启动 StorageNode 时生成或申请 node_id，写入本地 identity；重启同一 data_dir 后 node_id 保持不变；删除或损坏 identity 时系统报告明确错误或进入受控重新注册流程。

**Acceptance Scenarios**:

1. **Given** StorageNode 的 data_dir 不存在 node.identity，**When** 节点首次启动并注册成功，**Then** 系统分配 node_id 并在本地持久化 identity。
2. **Given** StorageNode 的 data_dir 已存在 node.identity，**When** 节点重启，**Then** 节点复用原 node_id，不得重新生成身份。
3. **Given** Raft MetadataNode 首次部署，**When** 配置生成流程创建集群配置，**Then** raft_id 与 node_id 被稳定记录；运行时注册到 ViewNode 不会改变 Raft membership。

---

### User Story 5 - 遵守 Raft quorum 安全边界 (Priority: P1)

作为系统所有者，我希望 Raft MetadataNode 的 commit 和 leader election 始终基于已提交 membership 的 voter 总数计算 quorum，即使当前存活节点减少，也不能为了可用性降低 quorum。

**Why this priority**: 对象 manifest 和 commit 状态是强一致元数据；错误缩小 quorum 会造成 split-brain 和对象可见性错误。

**Independent Test**: 以 3 个 voter 启动 MetadataNode，停止 2 个节点后验证剩余 1 个节点无法合法选主、无法 commit 新对象；以 5 个 voter 启动时验证 quorum 为 3。

**Acceptance Scenarios**:

1. **Given** Raft membership 中有 3 个 voter，**When** 2 个 voter 不可用，**Then** 剩余 1 个 voter 不能 commit 新对象，也不能形成合法 leader。
2. **Given** Raft membership 中有 5 个 voter，**When** 最多 2 个 voter 不可用，**Then** 只要仍有 3 个 voter 可达，系统可以继续按 Raft 规则提交元数据。
3. **Given** 新 Raft 节点只注册到了 ViewNode，**When** 该节点尚未通过 Raft leader 提交 membership change，**Then** quorum 计算不得包含该节点作为 voter。

---

### User Story 6 - 故障、恢复与并发读写 (Priority: P3)

作为测试者或运维者，我希望系统在 StorageNode 故障、节点重启、未提交写入、并发上传下载和大文件传输下仍保持对象可见性、checksum 和身份稳定。

**Why this priority**: 第一阶段需要证明系统雏形不是一次性 happy-path demo，而是具备工业化测试入口和可恢复边界。

**Independent Test**: 运行端到端故障场景，包括 StorageNode 写入后重启、commit 前失败、并发上传下载、无健康 StorageNode、checksum mismatch，并检查最终对象状态和文件校验结果。

**Acceptance Scenarios**:

1. **Given** chunk 已写入 StorageNode 但 CommitObject 未成功，**When** 清理流程或重启恢复发生，**Then** 未提交数据不得成为可见对象，staging 数据应被识别并可安全清理。
2. **Given** StorageNode 写入 chunk 后重启，**When** Client 下载已提交对象，**Then** StorageNode 仍能按 manifest 返回 chunk，并通过 checksum 校验。
3. **Given** 多个客户端并发上传和下载不同对象，**When** 系统处于正常 quorum 和健康存储容量下，**Then** 已提交对象保持可下载且 checksum 正确。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 已有 Raft metadata control-plane 能提供强一致对象元数据、对象状态、版本、commit、snapshot 和 recovery 能力；本阶段必须复用并保护其 leader election、majority commit、snapshot/recovery、follower catch-up 和重启恢复语义。
- 已有 StorageNode data-plane 能执行真实 chunk 写入、读取、删除、checksum、落盘和恢复；本阶段必须复用并保护其 chunk durability 与校验语义。
- 已有 Raft 与 StorageNode 的单独能力不是本阶段重写对象；本阶段只补齐两者之间的端到端编排、服务发现、配置、身份和验收测试。

### Targeted Gaps Or Risks

- Client 目前缺少从服务发现到 metadata write plan、真实 chunk 写入、manifest commit、再到下载校验的一体化对象流程。
- 集群节点数量、地址、端口、路径和容量不能依赖代码常量或固定 demo 拓扑。
- ViewNode 的职责边界需要提前钉住：它是发现与观测组件，不是对象 manifest 存储，也不是 Raft membership 权威。
- Raft quorum 必须从已提交 membership 的 voter 总数计算，不能按当前存活节点动态缩小。
- node_id 必须自动分配或由配置生成流程分配，并在本地持久化，避免节点重启后身份漂移。
- 真实文件数据必须严格留在 StorageNode data-plane，不能进入 Raft log、Raft snapshot 或 metadata state machine snapshot。
- 跨平台 durability、路径、rename/flush 差异需要在设计与测试中明确，不允许 required durability operation 静默降级。

### Non-Goals

- 不恢复旧 KV demo，不把本阶段实现方向退回 key-value 示例。
- 不让真实文件 payload、完整 chunk 数据或大文件内容进入 Raft log、Raft snapshot 或 metadata snapshot。
- 第一阶段不要求 ViewNode 自身具备强一致复制；ViewNode 可以作为服务发现与状态观测组件。
- 第一阶段不强制实现运行时 AddRaftNode、RemoveRaftNode、PromoteLearner 的完整 Raft membership change；但必须保留接口边界和后续设计入口。
- ViewNode 不得直接修改 Raft membership，不得直接把 Raft 新节点变成 voter，不得成为 membership 权威。
- 不改变已存在持久化格式、协议语义、公共 API 行为、类名、函数名或命名空间，除非后续实现任务单独明确并通过兼容性计划。
- 不在本阶段引入 erasure coding、跨机房复制、多租户权限、对象生命周期策略、加密、配额计费或完整 S3 兼容语义。

### Platform Scope

- Linux 是第一阶段主要验证平台，必须覆盖端到端上传下载、quorum 不足、StorageNode 重启、checksum mismatch、并发读写和基础故障场景。
- Windows 必须作为支持目标纳入配置、路径、启动入口、文件持久化和 flush/rename 语义设计；无法与 Linux 等价的 durability 行为必须记录明确 contract 或返回明确错误。
- macOS 不作为第一阶段强制验收平台，但共享代码设计不得引入无隔离的 Linux-only 假设；需要记录后续验证边界。

### Edge Cases

- Raft membership 为 3 个 voter 时 2 个节点不可用：不能选主，不能 commit 新对象，已提交对象的 manifest 查询只能在合法 leader 或只读策略允许的边界内进行。
- StorageNode 在 write plan 生成后、chunk 写入前变为不健康：Client 或 MetadataNode 必须能失败、重试或重新规划，不能把失败 chunk 记录为已提交副本。
- StorageNode 在 chunk 写入成功后、CommitObject 前崩溃：对象不能可见，恢复后 staging 或 orphan chunk 需要可识别并清理。
- CommitObject 成功但 Client 未收到响应：重试必须具备幂等边界，不能重复创建冲突版本或破坏 manifest。
- 下载过程中某个 chunk checksum 不匹配：下载失败并报告校验错误，不能输出被声明为成功的文件。
- ViewNode 不可用：已拿到 manifest 的下载不应依赖 ViewNode 继续参与；新的发现请求应失败或使用明确配置的备用 ViewNode。
- node.identity 缺失、损坏或与配置冲突：节点必须拒绝以错误身份静默启动，并给出可诊断错误。
- 可用 StorageNode 容量不足：MetadataNode 必须拒绝生成不可完成的 placement，或返回明确的容量不足结果。
- 并发上传同一对象名：必须通过版本、条件提交或明确冲突规则保证最终可见对象状态一致。
- 大文件上传下载：必须按 chunk 流式处理，不允许要求一次性把完整文件载入内存。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST connect Client, ViewNode, Raft MetadataNode, and StorageNode into a real object upload/download flow.
- **FR-002**: System MUST keep real file payload exclusively in the StorageNode data-plane; Raft MUST store only metadata such as write plan, placement, object manifest, chunk manifest, checksum, size, version, and commit state.
- **FR-003**: ViewNode MUST provide node registration, service discovery, endpoint lookup, capacity summary, health state, heartbeat timestamp, and liveness observation for ViewNode, MetadataNode, and StorageNode records.
- **FR-004**: ViewNode MUST NOT directly operate StorageNode data, store object manifest as the consistency authority, participate in object commit decisions, or modify Raft membership.
- **FR-005**: Cluster topology MUST be configuration-driven for ViewNode, Raft MetadataNode, and StorageNode count, address, port, data_dir, capacity, and startup role.
- **FR-006**: ViewNode count MUST be configurable; first-stage deployments may run one or more ViewNodes without requiring ViewNode consensus.
- **FR-007**: Raft MetadataNode count MUST be configurable for initial deployments including 1, 3, 5, and 7 voters, with commit quorum and election quorum calculated from the committed Raft membership voter total.
- **FR-008**: System MUST NOT reduce Raft quorum based on currently alive nodes; if membership has 3 voters and only 1 is alive, the system MUST NOT commit new metadata and MUST NOT elect a legal leader.
- **FR-009**: StorageNode count MUST be configurable and expandable by adding configuration and starting new nodes, without code changes.
- **FR-010**: Placement MUST select only StorageNodes that are currently healthy, reachable according to ViewNode observation, and have enough available capacity for the planned chunks.
- **FR-011**: Client upload MUST first discover MetadataNode addresses through ViewNode, request a write plan from Raft MetadataNode, write real chunks to StorageNode, receive chunk_id/node_id/size/checksum results, and then commit object metadata through Raft MetadataNode.
- **FR-012**: Objects MUST remain invisible to normal reads until CommitObject has been committed through Raft and object state is COMMITTED.
- **FR-013**: Client download MUST discover MetadataNode addresses through ViewNode, retrieve an object manifest from Raft MetadataNode, read chunks from StorageNode according to the manifest, verify each chunk checksum, reconstruct the file, and verify final file SHA-256.
- **FR-014**: StorageNode chunk writes MUST preserve staging -> durable flush -> publish semantics, and recovery MUST distinguish published chunks from incomplete staged data.
- **FR-015**: System MUST support node_id assignment for view/meta/store node types with local identity persistence; restarting with the same data_dir MUST preserve node identity.
- **FR-016**: StorageNode first startup without node.identity MUST obtain or generate a stable node_id through the approved identity flow and persist it before accepting chunk placement.
- **FR-017**: Raft MetadataNode node_id and raft_id MUST be generated through initial cluster configuration or a controlled config generator in the first stage; runtime ViewNode registration MUST NOT imply Raft membership.
- **FR-018**: Raft new-node discovery MUST distinguish ViewNode registration from Raft membership change: only Raft leader-submitted and Raft log-committed membership changes may add voters.
- **FR-019**: First-stage design MUST reserve AddRaftNode, RemoveRaftNode, learner catch-up, and PromoteLearner boundaries as future advanced capabilities, even if not fully implemented now.
- **FR-020**: The repository MUST provide independent startup apps for view_node_app, raft_metadata_node_app or metadata_node_app, storage_node_app, and storage_client; storage_bench is optional.
- **FR-021**: Startup parameters SHOULD be consistent across apps, including config path, node_id override when allowed, data_dir override when safe, and role-specific endpoint selection.
- **FR-022**: Configuration generation MUST avoid requiring users to manually maintain fixed node names whenever system assignment is possible, while preserving stable identity after first assignment.
- **FR-023**: End-to-end tests MUST cover real file upload/download with SHA-256 equality, quorum insufficiency, StorageNode failure, StorageNode restart, concurrent upload/download, checksum mismatch, and uncommitted data cleanup.
- **FR-024**: Large object transfer MUST use chunked or streaming behavior so memory use is bounded by chunk/concurrency settings rather than full object size.
- **FR-025**: Any Linux-specific validation path MUST be explicitly labeled and MUST identify the Windows fallback, adaptation, or follow-up expectation.
- **FR-026**: Any required durability operation MUST define expected Linux and Windows behavior and MUST NOT silently succeed as a no-op on unsupported platforms.
- **FR-027**: Each newly introduced module MUST include or plan a module-notes.md describing module responsibility, key structures, key functions, and misuse boundaries.
- **FR-028**: Cross-task risks or design smells discovered during implementation MUST be recorded in a feature risk register or task-reports file, not appended as execution logs to high-frequency documents.

### Key Entities *(include if feature involves data)*

- **ClusterConfig**: Describes the intended cluster topology, node roles, endpoint bindings, data directories, capacity hints, chunk sizing, heartbeat policy, and initial Raft membership.
- **NodeIdentity**: Stable local identity for a node; includes node_id, node type, optional raft_id, creation metadata, and durability state. It must survive restart.
- **NodeRegistration**: ViewNode record for a registered node; includes node_id, role, endpoint, capacity, health, last heartbeat time, and role-specific status.
- **ViewNode**: Service discovery and observability component for node registration, endpoint lookup, heartbeat tracking, and status display. It is not object metadata authority.
- **Raft MetadataNode**: Strongly consistent metadata node that owns write plan decisions, object manifest, object state transitions, version visibility, and committed Raft membership.
- **StorageNode**: Data-plane node that stores real chunks, verifies checksum, publishes durable chunks, serves chunk reads, deletes chunks, and recovers local chunk catalog after restart.
- **WritePlan**: Metadata proposal for an object upload; includes object key, version or write token, chunk layout, selected placement, expected sizes, checksum requirements, and expiration/cleanup boundary.
- **Placement**: Assignment of each planned chunk replica to one or more healthy StorageNodes with sufficient capacity.
- **ChunkManifest**: Committed record for a chunk; includes chunk_id, node_id, offset/order, size, checksum, and replica status.
- **ObjectManifest**: Committed object metadata; includes object key, version, total size, file checksum, ordered chunk manifests, object state, and commit timestamp or logical commit marker.
- **RaftMembership**: Raft-authoritative committed voter/learner configuration. ViewNode may display it but cannot authoritatively mutate it.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With a configured cluster of 1 ViewNode, 3 Raft MetadataNodes, and 3 StorageNodes, users can upload and download a 64 MiB file with identical SHA-256 before and after transfer in at least 10 consecutive runs.
- **SC-002**: A 3-voter metadata cluster reports quorum 2, a 5-voter cluster reports quorum 3, and a 7-voter cluster reports quorum 4; tests prove quorum is not lowered when nodes fail.
- **SC-003**: In a 3-voter metadata cluster with 2 voters unavailable, attempts to commit a new object fail without exposing a partially committed object.
- **SC-004**: Restarting every StorageNode with the same data_dir preserves node_id and allows previously committed objects to be downloaded with checksum verification.
- **SC-005**: Adding a new StorageNode by configuration and startup only, without code changes, makes it visible through ViewNode and eligible for new placement after healthy heartbeats.
- **SC-006**: Concurrent upload/download validation with at least 100 client operations completes with every successfully committed object passing final SHA-256 verification.
- **SC-007**: Checksum mismatch injection causes the affected download to fail with a clear integrity error in 100% of test runs, with no success result emitted for corrupted data.
- **SC-008**: Large-file transfer demonstrates bounded memory behavior proportional to chunk size and configured concurrency, not proportional to full file size.
- **SC-009**: Linux validation covers all first-stage acceptance scenarios; Windows startup/configuration/durability expectations are documented and at least smoke-tested where platform support exists.
- **SC-010**: No test artifact or inspection shows real file payload embedded in Raft log, Raft snapshot, or metadata snapshot.

## Assumptions

- First-stage deployments may use a single ViewNode for discovery and observation; multiple ViewNodes are configured as independent discovery endpoints without requiring ViewNode consensus.
- Initial Raft MetadataNode voter membership is generated before startup and remains static during the first implementation stage.
- Runtime AddRaftNode, RemoveRaftNode, learner catch-up, and PromoteLearner are reserved as later capabilities and must go through Raft leader and Raft log commit when implemented.
- StorageNode expansion is in scope for first-stage dynamic registration and placement eligibility; StorageNode removal and automated re-replication are not required in the first stage.
- The object model can use whole-object SHA-256 plus per-chunk checksum; exact checksum algorithm names may be finalized in implementation planning as long as integrity verification remains testable.
- Client-facing authentication, authorization, multi-tenant isolation, billing, lifecycle policy, and S3 compatibility are out of scope for this stage.
- Existing verified Raft and StorageNode correctness remains protected; this feature adds integration and operational surfaces rather than replacing those cores.
