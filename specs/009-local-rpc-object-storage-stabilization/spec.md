# Feature Specification: Local RPC Object Storage Stabilization

**Feature Branch**: `009-local-rpc-object-storage-stabilization`  
**Created**: 2026-06-11  
**Status**: Draft  
**Input**: User description: "009-local-rpc-object-storage-stabilization"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - ViewNode 自身长期保持可发现 (Priority: P1)

运维者启动单个 ViewNode 后，即使没有外部 StorageNode 或 MetadataNode 心跳，该 ViewNode 仍能持续刷新自己的观测状态，并在超过 liveness TTL 后保持 `LIVE`。

**Why this priority**: 008 报告明确指出 `status` 中 `view-1` 的 self-liveness 仍可能显示 `stale/dead`，这是 009 最直接的稳定性缺口。

**Independent Test**: 只启动或构造一个 ViewNode registry，运行超过 stale/suspect/dead TTL，检查自身 cluster view 记录仍为 `LIVE`；关闭 self refresh 后再验证 TTL 状态转换。

**Acceptance Scenarios**:

1. **Given** 单 ViewNode 已启动并把自己纳入 cluster view，**When** 运行时间超过 dead TTL，**Then** 自身 liveness 仍为 `LIVE`。
2. **Given** 单 ViewNode 的 self refresh 被显式停止，**When** 时间推进超过 stale/suspect/dead TTL，**Then** 自身状态依次转为 `STALE`、`SUSPECT`、`DEAD`。
3. **Given** ViewNode 重启生成新的 process incarnation，**When** 旧进程残留 heartbeat 或旧 snapshot 到达，**Then** 旧 incarnation 不得覆盖新状态。

---

### User Story 2 - 多 ViewNode active-active discovery (Priority: P1)

运维者可以运行至少 2 个 ViewNode，节点和客户端配置多个 ViewNode seed；任一 ViewNode 故障时，客户端仍能通过另一个 ViewNode 发现 Metadata leader 与 LIVE StorageNode。

**Why this priority**: 009 目标要求避免 discovery 单点，同时保持 ViewNode 只是 discovery / observation，不引入新的强一致配置中心。

**Independent Test**: 启动两个 ViewNode，向其中一个注册 metadata/storage facts，通过 peer sync 在另一个 ViewNode 查询到一致的 observed registry；停止一个 ViewNode 后，客户端从另一个 ViewNode 完成 discovery。

**Acceptance Scenarios**:

1. **Given** `view-1` 与 `view-2` 都在运行，**When** StorageNode 或 MetadataNode 向任一 ViewNode 注册，**Then** 两个 ViewNode 最终都能返回对应 observed state。
2. **Given** `view-1` 故障，**When** Client 从 `view-2` 查询 cluster view，**Then** 仍能发现 Metadata leader hint 和可写 StorageNode 候选。
3. **Given** `view-1` 重启产生新 incarnation，**When** `view-2` 合并来自旧 incarnation 的较新 wall-clock observed_time，**Then** 合并规则仍保留新 incarnation 状态。

---

### User Story 3 - StorageNode 运行中动态加入并参与后续写入 (Priority: P1)

运维者可以在本地 RPC 集群运行中启动新的 StorageNode。该节点首次启动时创建本地持久 identity，向 ViewNode 注册并 heartbeat，随后新对象 upload placement 可以选择它。

**Why this priority**: 008 已证明配置内 6 个 StorageNode 能参与真实 upload/download；009 需要把静态配置能力推进到运行中加入。

**Independent Test**: 在已有 2 ViewNode + 3 MetadataNode + N StorageNode 集群运行时启动新 StorageNode，等待 discovery 可见，再上传新对象并断言 write plan / chunk placement 包含或可使用该新节点；旧对象不要求 rebalance。

**Acceptance Scenarios**:

1. **Given** 新 StorageNode 的 `identity_file` 不存在，**When** 节点启动，**Then** 本地创建并原子发布 storage identity，重启后复用同一 `node_id`。
2. **Given** 新 StorageNode 已向 ViewNode 注册并持续 heartbeat，**When** Client 或 Metadata leader 请求 LIVE StorageNode 列表，**Then** 新节点出现在 discovery / placement 候选中。
3. **Given** 新 StorageNode 加入后，**When** 上传新对象，**Then** 新对象可以使用该 StorageNode 的 data-plane 写入路径，且不影响 Raft quorum。

---

### User Story 4 - MetadataNode 动态 learner join 与奇数 voter 扩容 (Priority: P1)

运维者可以在 3 voter Metadata/Raft 集群运行中启动新的 MetadataNode。新节点只能先成为 learner / non-voter，追日志和 snapshot；单个 learner ready 时不得 promote 成 4 voters，两个 ready learners 可以通过安全批量 membership change 扩到 5 voters。

**Why this priority**: 008 明确没有完成运行中动态新增 Metadata/RaftNode 并自动纳入 Raft membership；009 必须区分 discovery registration 与 consensus membership join。

**Independent Test**: 在 3 voter 集群中运行中加入 1 个 MetadataNode，验证 learner 追日志但 quorum 仍为 2 且不能投票；再加入第 2 个 learner，两个 ready 后通过批量 promote 达到 5 voters，quorum 变为 3，且没有任何 committed 4-voter 配置。

**Acceptance Scenarios**:

1. **Given** 3 committed voters，**When** 1 个动态 MetadataNode join，**Then** 它进入 learner / pending learner，能接收 AppendEntries/InstallSnapshot，但不投票、不计 quorum、不能成为 leader。
2. **Given** 3 voters + 1 ready learner，**When** 请求单独 promote，**Then** 返回 `blocked_by_even_voter_count` / `waiting_for_pair` / `need_another_ready_learner` 等明确状态，learner 继续追赶。
3. **Given** 3 voters + 2 ready learners，**When** 执行安全 batch promote / joint consensus / batched membership change，**Then** committed voter 集合直接变为 5 voters，quorum 从 2 变为 3，中间不提交 4 voters。

---

### User Story 5 - 节点身份生命周期可恢复且可诊断 (Priority: P2)

开发者和运维者可以清楚区分 `identity_file`、长期 `node_id`、`raft_id`、进程 `incarnation / boot epoch`、heartbeat `sequence` 与 `observed_time` 的职责，节点首次启动能创建身份，重启能复用身份，冲突和损坏能 fail-fast。

**Why this priority**: 动态节点加入依赖稳定身份；如果身份模型不收口，ViewNode merge、防旧 incarnation 覆盖、Metadata membership 权威都会变得不可靠。

**Independent Test**: 对 StorageNode、ViewNode、Metadata bootstrap voter、Metadata dynamic join candidate 分别验证首次创建、重启复用、cluster_id mismatch、node_type mismatch、损坏文件和旧 incarnation 覆盖防护。

**Acceptance Scenarios**:

1. **Given** `identity_file` 不存在，**When** StorageNode 或 ViewNode 首次启动，**Then** 创建本地持久身份，而不是要求 ViewNode 预先分配。
2. **Given** MetadataNode 是 dynamic join，**When** 它首次启动，**Then** 本地身份状态只能是 joining/candidate，不能自己成为 voter。
3. **Given** `identity_file` 损坏或与配置不匹配，**When** 节点启动，**Then** 明确失败并给出 cluster_id/node_type/node_id/raft_id 维度诊断。

---

### User Story 6 - 本地 RPC example 与集成验证覆盖稳定性矩阵 (Priority: P2)

开发者可以用现有本地 RPC example 和 CTest 入口验证 009 的动态加入、ViewNode 故障、重启、重复注册、TTL、旧 incarnation、odd voter 等稳定性场景。

**Why this priority**: 009 不是最小 demo，必须把动态节点系统的端到端路径和故障矩阵纳入可重复验证。

**Independent Test**: 扩展 `examples/object-storage-local-3meta-6store` 或新增同类 009 example，运行 2 ViewNode + 3 MetadataNode + 多 StorageNode，执行运行中加入 StorageNode、加入 1/2 Metadata learners、ViewNode failover、identity restart 和真实 upload/download。

**Acceptance Scenarios**:

1. **Given** 009 local RPC example 已启动，**When** 停止一个 ViewNode，**Then** Client 仍能通过另一个 ViewNode 完成 discovery 和 upload/download。
2. **Given** 集群运行中新增 StorageNode，**When** 执行后续 roundtrip，**Then** 新对象能使用新的 StorageNode 候选。
3. **Given** 集群运行中新增 1 个 Metadata learner 再新增第 2 个 learner，**When** 两者 ready 后执行 batch promote，**Then** committed voter count 保持奇数并扩到 5。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 指定报告位于 `specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`。
- 报告确认当前真实 local RPC object storage example 位于 `examples/object-storage-local-3meta-6store`。
- 报告确认当前 example 使用 1 个 ViewNode、3 个 MetadataNode、6 个 StorageNode，客户端通过 `storage_client` 走真实 gRPC/RPC，测试文件目录为 `tests/test_file`。
- 报告确认当前修复涉及：
  - `apps/metadata_node_app.cpp`
  - `modules/raft/service/metadata_service_impl.cpp`
  - `modules/store/transfer/object_transfer.cpp`
- 报告确认验证 target 包括 `view_node_app`、`metadata_node_app`、`storage_node_app`、`storage_client`、`raft_metadata_client`。
- 报告确认当前真实 RPC 验证脚本入口包括：
  - `examples/object-storage-local-3meta-6store/qidong.sh`
  - `examples/object-storage-local-3meta-6store/tingzhi.sh`
  - `examples/object-storage-local-3meta-6store/rpc_demo.sh status`
  - `examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`
- 报告确认 roundtrip 覆盖 `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> 最终文件比对`，且 4 个真实文件最终 `cmp` 通过。
- 报告确认当前剩余问题：`status` 中 `view-1` self-liveness 仍可能显示 `stale/dead`，原因是 ViewNode 自身 registry 记录没有持续 self-heartbeat。

补充现状勘察确认：

- `tests/CMakeLists.txt` 中当前相关 CTest target / label 包括：
  - `test_integrated_object_storage_e2e` / custom target `integrated_object_storage_e2e`，label `integrated-object-storage;integrated-object-storage-e2e;storage-transfer;platform-neutral;linux-primary-diagnosis`
  - `test_integrated_object_storage_quorum` / custom target `integrated_object_storage_quorum`，label `integrated-object-storage;integrated-object-storage-quorum;platform-neutral`
  - `test_integrated_object_storage_recovery` / custom target `integrated_object_storage_recovery`，label `integrated-object-storage;integrated-object-storage-recovery;storage-transfer;storage-node;storage-node-recovery;durability-boundary;linux-primary-diagnosis`
  - `test_integrated_object_storage_concurrency` / custom target `integrated_object_storage_concurrency`，label `integrated-object-storage;integrated-object-storage-concurrency;storage-transfer;storage-node;storage-node-concurrency;platform-neutral;linux-primary-diagnosis`
  - `test_view_node_discovery`，label `integrated-object-storage;view-node;platform-neutral`
  - `test_node_identity`，label `integrated-object-storage;node-identity;platform-neutral;durability-boundary;windows-adaptation`
  - `cluster_config_test`，label `integrated-object-storage;platform-neutral`
  - `storage_heartbeat_registry`，label `storage-node;platform-neutral`
- 当前 ViewNode registry / liveness 测试位于 `tests/view_node_discovery_test.cpp`。
- 当前 StorageNode 注册 / heartbeat / liveness 测试位于 `tests/storage_heartbeat_registry_test.cpp`，StorageNode service/client 测试位于 `tests/storage_node_service_test.cpp` 和 `tests/storage_node_client_test.cpp`。
- 当前 node identity 测试位于 `tests/node_identity_test.cpp`，实现位于 `modules/cluster/node_identity.h` 和 `modules/cluster/node_identity.cpp`。
- 当前 ViewNode registry 实现位于 `modules/view/view_registry.h` 和 `modules/view/view_registry.cpp`，gRPC adapter/client 位于 `modules/view/view_service_impl.*` 和 `modules/view/view_client.*`。
- 当前 app 启动路径位于 `apps/view_node_app.cpp`、`apps/metadata_node_app.cpp`、`apps/storage_node_app.cpp`、`apps/storage_client.cpp`。
- 当前 Raft bootstrap / election / replication / commit / snapshot catch-up / restart 测试入口包括：
  - `tests/test_raft_election.cpp`
  - `tests/test_raft_log_replication.cpp`
  - `tests/test_raft_commit_apply.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/metadata_failover_test.cpp`
  - `tests/metadata_client_scenario_test.cpp`
  - `tests/integrated_object_storage_quorum_test.cpp`
- 当前 Raft runtime membership 仍由 `raftdemo::NodeConfig::peers` 表示；`CommittedMembershipQuorumSummary` 已暴露只读诊断，但 learner 在 `RaftNode` 内部未下沉为运行时 membership。
- 当前 `ClusterConfig` 已有 `cluster_id`、`node_id`、Metadata `raft_id`、初始 `voter_raft_ids` / `learner_raft_ids`、`membership_epoch`，但没有 process `incarnation / boot epoch` 字段。
- 当前 ViewNode / StorageNode registry 有 heartbeat `sequence` 和 `observed_at_unix_ms`，但没有 incarnation-aware merge。
- 当前 ViewNode registry 是内存型；`modules/view/module-notes.md` 明确未实现 registry 持久化、ViewNode HA/复制。
- 当前 `view_node_app.cpp` 启动时只把自己 `RegisterNode` 到本地 registry 一次，没有持续 self refresh loop。
- 当前 `metadata_node_app.cpp` 会向配置内多个 ViewNode 注册并 heartbeat；`storage_node_app.cpp` 会向一个可用 ViewNode 注册并 heartbeat，失败后重试/切换。

### Targeted Gaps Or Risks

- ViewNode 自身状态没有持续刷新，导致健康 ViewNode 被自己的 TTL 判为 stale/dead。
- 多 ViewNode active-active discovery 和 registry peer sync 尚未实现，discovery 仍可能单点。
- ViewNode merge 规则缺少 process incarnation / boot epoch，旧状态可能借由更晚 observed_time 覆盖新进程。
- StorageNode 动态加入需要从静态配置拓扑转为 seed discovery 语义，且不能影响 Raft quorum。
- MetadataNode 动态加入需要正式的 consensus membership join，不能简化成 ViewNode 注册。
- Raft learner、pending learner、ready-to-promote、waiting-for-pair、batch promote / joint consensus 语义尚未形成完整实现与验证。
- Committed voter count 必须始终保持奇数，不能通过短暂 4/6 voters 过渡。
- 009 验证必须从单次静态启动扩展到运行中加入、故障、重启、重复注册、TTL、旧 incarnation 和 odd voter 防护。

### Non-Goals

- 不做 StorageNode 自动 rebalance。
- 不做自动删除 Raft voter。
- 不让 ViewNode 成为 Raft membership authority。
- 不要求 ViewNode registry 强一致。
- 不把所有节点配置成完整静态拓扑。
- 不要求动态加入节点立即承载旧对象。
- 不引入复杂安全认证；可以预留本地开发 join token / cluster_id 校验字段。
- 不做跨机房、跨公网、NAT 穿透、TLS 证书体系。
- 不做生产级负载均衡器。
- 不做 StorageNode 后台数据迁移 / balance / repair。
- 不做自动缩容。
- 不做自动把 DEAD MetadataNode 从 Raft membership 删除。
- 不让 ViewNode 成为全局 ID 分配权威。
- 不把 `identity_file` 缺失视为启动错误；第一次启动应能创建身份，除非配置明确要求必须预置。
- 不允许 committed Raft voter membership 出现偶数 voter count。
- 不允许为了过渡而短暂提交 4 voter / 6 voter 这种偶数 voter 配置。

### Platform Scope

- Linux 是 009 的主要本地验证平台，local RPC example、动态加入、ViewNode failover、Raft learner catch-up 和 odd voter 场景都应给出 Linux 可执行验证路径。
- Windows/macOS 作为设计目标保留身份、路径、durability、启动参数、registry 持久化语义的兼容边界；没有实机结果时必须标记待测。
- 任何 required durability operation 不允许在非 Linux 平台 silent no-op success；必须等价实现、返回明确错误，或在 durability contract 记录较弱保证。

### Edge Cases

- ViewNode 本地 self refresh 正常运行但没有外部节点心跳。
- ViewNode self refresh 停止后 TTL 正确转为 stale/suspect/dead。
- 旧 incarnation heartbeat、旧 registry snapshot 或旧 observed_time 晚到。
- 同 node_id 不同 endpoint、同 endpoint 不同 node_id、同 data_dir_fingerprint 冲突。
- `identity_file` 首次不存在、损坏、cluster_id mismatch、node_type mismatch、raft_id mismatch。
- StorageNode 重启复用 node_id 但生成新 incarnation。
- StorageNode 注册到一个 ViewNode 后，另一个 ViewNode 故障或 lagging。
- Dynamic MetadataNode 重复 join、leader change、pending membership change 冲突。
- 3 voters + 1 learner ready 但不能单独 promote。
- 3 voters + 2 learners batch promote 期间 leader 故障。
- Snapshot 已压缩日志后 learner 需要 InstallSnapshot catch-up。
- ViewNode observed_time 较新但 incarnation 较旧。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: ViewNode MUST maintain a periodic self refresh loop for its own observed state when it includes itself in cluster view.
- **FR-002**: A healthy running ViewNode MUST NOT become `STALE`, `SUSPECT`, or `DEAD` only because no external node heartbeat refreshed it.
- **FR-003**: ViewNode self state MUST include `node_id`, endpoint, process incarnation / boot epoch, sequence, observed_time, health, and liveness.
- **FR-004**: ViewNode MUST support at least two active nodes for discovery availability.
- **FR-005**: ViewNode peers MUST synchronize observed registry / cluster view state using deterministic merge semantics.
- **FR-006**: ViewNode registry merge MUST prioritize higher incarnation / boot epoch over lower incarnation.
- **FR-007**: ViewNode registry merge MUST prioritize higher sequence within the same incarnation.
- **FR-008**: `observed_time` MUST be used for TTL / liveness judgment and MUST NOT alone override a newer incarnation.
- **FR-009**: ViewNode registry persistence or restart recovery boundary MUST be explicitly defined before peer sync is considered complete.
- **FR-010**: ViewNode MUST remain discovery-only / observation-only and MUST NOT change Raft voter / learner membership.
- **FR-011**: StorageNode first start MUST create a local persistent identity when `identity_file` does not exist.
- **FR-012**: StorageNode restart MUST reuse the existing long-lived `node_id` and create a fresh process incarnation.
- **FR-013**: StorageNode heartbeat MUST report node_id, incarnation, sequence, endpoint, capacity, load, health, disk pressure, and writable status.
- **FR-014**: StorageNode dynamic registration MUST make the node visible to subsequent discovery and placement for new objects.
- **FR-015**: StorageNode dynamic registration MUST NOT affect Raft quorum, election, or committed metadata manifest.
- **FR-016**: MetadataNode dynamic join MUST start as joining/candidate and MUST NOT decide locally that it is voter.
- **FR-017**: Dynamic MetadataNode MUST register observable metadata facts with ViewNode before or while discovering the current Metadata leader.
- **FR-018**: Dynamic MetadataNode MUST discover the current Metadata leader through ViewNode candidates and still handle MetadataService `NOT_LEADER`.
- **FR-019**: Metadata leader MUST expose a JoinMetadataCluster flow or equivalent membership authority path for dynamic MetadataNode join.
- **FR-020**: Metadata leader MUST validate cluster_id, node_id, endpoint, duplicate join, and pending membership change before accepting a join.
- **FR-021**: New MetadataNode MUST first enter learner / non-voter membership through a committed Raft configuration log entry.
- **FR-022**: Learner MUST receive AppendEntries and InstallSnapshot and advance match_index / applied_index / commit_index as appropriate.
- **FR-023**: Learner MUST NOT vote, count toward quorum, or become leader before promote-to-voter is committed.
- **FR-024**: Promote-to-voter MUST only become effective through committed Raft membership change.
- **FR-025**: Committed Raft voter membership MUST always contain an odd number of voters.
- **FR-026**: A single ready learner that would make voter count even MUST remain pending / waiting-for-pair and continue catch-up.
- **FR-027**: The system MUST return a clear blocked status for single learner promote attempts that would create 4 or 6 voters.
- **FR-028**: Two ready learners MAY be promoted together only through a safe batch membership change / joint consensus / batched promote path that never commits an even voter count.
- **FR-029**: Quorum calculation MUST always use committed voter membership and MUST exclude learners.
- **FR-030**: Leader election MUST only run among committed voters.
- **FR-031**: Identity model MUST distinguish long-lived `node_id`, Raft `raft_id`, process `incarnation / boot epoch`, heartbeat `sequence`, and `observed_time`.
- **FR-032**: `identity_file` MUST be defined as a local persistent identity path, not as a file that must pre-exist and not as a ViewNode-issued identity.
- **FR-033**: Metadata bootstrap voter identity MAY be created from bootstrap config with fixed node_id / raft_id / voter state.
- **FR-034**: Metadata dynamic join identity MUST start with membership_state joining/candidate and optional/provisional raft_id until committed membership defines its role.
- **FR-035**: Existing object upload/download behavior verified by 008 report MUST remain functional.
- **FR-036**: `CreateWritePlan`, `CommitObject`, `HeadObject`, and Metadata authority paths MUST remain leader/quorum controlled.
- **FR-037**: Quorum MUST NOT shrink based on live nodes or ViewNode observations.
- **FR-038**: Local RPC example MUST cover 2 ViewNodes, 3 initial Metadata voters, multiple StorageNodes, dynamic StorageNode join, and dynamic Metadata learner join.
- **FR-039**: Local RPC example MUST verify ViewNode failover where Client continues discovery through the surviving ViewNode.
- **FR-040**: Tests MUST cover single ViewNode self refresh beyond TTL.
- **FR-041**: Tests MUST cover ViewNode peer sync and old incarnation not overriding new incarnation.
- **FR-042**: Tests MUST cover StorageNode restart, duplicate registration, stale heartbeat, and capacity/health/load report merge.
- **FR-043**: Tests MUST cover MetadataNode duplicate join, leader change during learner catch-up, and pending membership change rejection.
- **FR-044**: Tests MUST cover learner not participating in quorum/election and not becoming leader.
- **FR-045**: Tests MUST prove 3 voters + 1 learner cannot commit 4 voters.
- **FR-046**: Tests MUST prove 3 voters + 2 ready learners can reach 5 voters without a committed 4-voter configuration, if batch promote is implemented.
- **FR-047**: Validation advice MUST prefer targeted build/test targets and existing CMake/CTest paths over standalone compiler invocations.
- **FR-048**: Test logs MUST be saved locally and summarized according to project test-log rules.
- **FR-049**: Any Linux-specific validation path MUST be labeled, with Windows/macOS fallback or pending validation recorded.
- **FR-050**: Any change affecting persistence, snapshot, restart recovery, replication, or membership MUST state crash/restart expectations.

### Key Entities *(include if feature involves data)*

- **NodeIdentity**: Persistent local identity with cluster_id, node_type, node_id, optional raft_id, created_at, persistent_generation/source, and validation state.
- **ProcessIncarnation**: Per-process startup identity / boot epoch generated on each start; used to prevent old process state from overwriting new process state.
- **HeartbeatSequence**: Monotonic sequence within one incarnation; used to order heartbeat/report updates.
- **ObservedState**: ViewNode registry entry containing identity, endpoint, incarnation, sequence, observed_time, liveness, health, capacity/load, and role-specific facts.
- **ViewRegistrySnapshot**: Serializable/syncable view of observed registry; mergeable but not strongly consistent.
- **StorageNodeRegistration**: Discovery registration for data-plane nodes; affects placement candidates for future objects only.
- **MetadataJoinRequest**: Request from dynamic MetadataNode to Metadata leader to join as learner.
- **LearnerState**: Non-voter Raft member that receives log/snapshot replication and tracks catch-up progress.
- **PendingLearnerSet**: Ready learners blocked from individual promote because odd voter invariant would be violated.
- **MembershipChangeBatch**: Committed Raft membership change that can promote a safe set of ready learners while preserving odd voter count.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A single ViewNode running for at least 2x configured dead TTL keeps its own cluster view entry `LIVE` while self refresh is enabled.
- **SC-002**: With two ViewNodes, stopping one ViewNode still allows client discovery and a new object roundtrip through the surviving ViewNode.
- **SC-003**: A StorageNode started after the cluster is already serving requests becomes visible to discovery and can be used by at least one subsequent new object upload.
- **SC-004**: In a 3-voter Metadata/Raft cluster, adding one learner keeps quorum at 2 and never commits a 4-voter configuration.
- **SC-005**: In a 3-voter Metadata/Raft cluster, adding two ready learners can reach 5 committed voters with quorum 3 and no committed even-voter intermediate state.
- **SC-006**: All identity tests for first start, restart reuse, mismatch, corrupt file, and type-specific raft_id rules pass on Linux; Windows-specific durability gaps are clearly marked if not verified.
- **SC-007**: The local RPC example validates status and roundtrip with real files after dynamic join/failover extensions, with logs stored under the feature task-report or example log locations and summarized only.

## Assumptions

- 009 uses the current C++20/gRPC/Protobuf/GoogleTest/CMake repository structure and does not replace the existing Raft, metadata, ViewNode, StorageNode, or transfer modules.
- Current static 008 path remains the compatibility baseline: `examples/object-storage-local-3meta-6store` with 1 ViewNode, 3 Metadata voters, 6 StorageNodes, and real `storage_client` roundtrip.
- ViewNode peer sync is eventually consistent observed-state synchronization, not linearizable consensus.
- Dynamic MetadataNode promote may require adding batch membership change or joint consensus before 3 voters + 2 learners can safely become 5 voters.
- If batch promote is not completed in an early milestone, learner join and catch-up can still be delivered while promote remains explicitly blocked by odd-voter invariant.
- StorageNode dynamic join does not rebalance existing objects; only future placement may use the new node.
