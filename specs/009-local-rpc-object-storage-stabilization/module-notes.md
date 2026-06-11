# Module Notes: 009 Local RPC Object Storage Stabilization

## identity / cluster config

已确认入口：

- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`
- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `tests/cluster_config_test.cpp`

职责：

- `node_identity.*` 负责本地持久身份 `node.identity` 的路径解析、load/store/load-or-create、期望身份匹配校验、冲突/损坏 fail-fast、durability publish 边界。
- `cluster_config.*` 负责 cluster json 的生成、加载、按 `role + node_id` 精确解析、初始 voter/learner membership 校验和初始 quorum 诊断。
- `tests/node_identity_test.cpp` 当前已覆盖首次创建、重启复用、mismatch、corrupt、Metadata `raft_id` 边界。
- `tests/cluster_config_test.cpp` 当前已覆盖初始 voter 奇数约束、learner/voter 归属、role resolution、quorum helper。

输入：

- `data_dir` / `identity_file`。
- `ExpectedNodeIdentity` 中的 `cluster_id`、`node_id`、`node_type`、可选 `raft_id`、可选 `source`。
- cluster config 中的 `view_nodes`、`metadata_nodes`、`storage_nodes`、`initial_raft_membership`。

输出：

- 本地 durable `NodeIdentity`。
- 按 role 解析后的单节点配置。
- 仅供配置校验和诊断使用的初始 quorum / membership 摘要。

边界：

- `identity_file` 是节点自己的本地持久身份路径，不是 ViewNode 分配的租约，也不是运行时 membership authority。
- `identity_file` 首次缺失在 first-start 场景下是正常输入；当前 `LoadOrCreateNodeIdentity` 已按“缺失则创建、存在则校验复用”工作。
- 重启必须复用长期 `node_id`；`tests/node_identity_test.cpp` 已把“重启不静默换 node_id”作为现有边界。
- 每次进程启动都必须生成新的 incarnation / boot epoch；这不是 `node.identity` 的长期字段，后续应在 app/heartbeat 路径生成，不能靠重写 durable `node_id` 表达。
- StorageNode / ViewNode 可以本地生成并持久化自己的 `node_id`。即使当前 `NodeIdentitySource` 里有 `kViewNodeAllocator` 命名，它也只是来源诊断，不能把 ViewNode 解释成全局 ID authority。
- Metadata bootstrap voter 与 Metadata dynamic join candidate 必须分开。
- Metadata bootstrap voter 可以从 bootstrap config 固定出 `node_id + raft_id + initial_role`。
- Metadata dynamic join 只能创建 joining/candidate identity；本地文件不能让自己直接成为 voter。
- `cluster_config.initial_raft_membership` 只描述初始 membership 边界，不是运行时 membership change 入口。

容易误用点：

- 把“本地已有 `node.identity`”误写成“已经加入 Metadata/Raft voter 集合”。
- 把 `cluster_config` 的初始 voter/learner 配置误写成后续 dynamic join / promote 的 authority。
- 把 per-process incarnation / sequence 持久化成长期身份的一部分，导致重启后无法区分旧进程与新进程。

## ViewNode

已确认入口：

- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`
- `modules/view/view_service_impl.h`
- `modules/view/view_service_impl.cpp`
- `modules/view/view_client.h`
- `modules/view/view_client.cpp`
- `tests/view_node_discovery_test.cpp`
- `apps/view_node_app.cpp`

职责：

- `view_registry.*` 当前负责 observed registry、register/heartbeat、TTL liveness、metadata/storage discovery、cluster view、冲突诊断。
- `view_service_impl.*` 负责 gRPC adapter，把 View RPC 映射到 registry。
- `view_client.*` 负责 register、heartbeat、discover、cluster view 的 RPC client 映射和 transport diagnostics。
- `apps/view_node_app.cpp` 当前负责 load/create identity、构建 registry、启动 gRPC server，并在启动时把本节点注册进 registry。
- `tests/view_node_discovery_test.cpp` 当前已覆盖 register、heartbeat sequence、防 stale/duplicate、TTL、leader hint、storage filter、真实 RPC adapter。

输入：

- Register/Heartbeat 请求中的 `cluster_id`、`node_id`、`node_type`、endpoint、data dir fingerprint、health、capacity、load、metadata observation。
- `stale_timeout` / `suspect_timeout` / `dead_timeout`。
- 本地时间源和 RPC adapter/client 输入。

输出：

- Metadata leader hint、metadata candidate 列表、storage candidate 列表、cluster view snapshot、冲突/过滤诊断。

边界：

- ViewNode 只负责 discovery / observation，不决定 Raft voter / learner membership。
- ViewNode 可以暴露 `metadata.membership_state` 这样的 observed facts，但这不是 committed membership authority。
- 009 必须有 self refresh；当前 `apps/view_node_app.cpp` 只在启动时 `RegisterNode` 一次，没有持续 self refresh loop，因此现状不能被误写成“已经满足 self liveness contract”。
- ViewNode peer sync 在 009 中只能是 observed registry 的最终一致同步，不是强一致配置中心，也不是 membership config store。当前这组文件里尚未确认专门的 peer sync loop / RPC 入口，后续必须扩展这些现有入口，而不是新造 authority 路径。
- merge 规则必须优先按 incarnation，再按 sequence。当前 registry 基线已具备 sequence / observed_at 防 stale 能力，但 009 后续要把旧进程与新进程隔离扩展到 incarnation-aware merge。
- `observed_time` / `observed_at_unix_ms` 只用于 TTL、liveness 和诊断，不能单独覆盖更高 incarnation 的状态。
- 如果 registry 继续保持内存型，重启后的恢复边界必须明确写成：靠 self refresh、node heartbeat、peer sync 重新收敛；不能把重启前 registry 当成 durable authority。

容易误用点：

- 把 ViewNode 中 `MetadataMembershipObservedState::kVoter` 当成真实 voter membership。
- 用 `observed_time` 较新去覆盖更高 incarnation 的 LIVE 状态。
- 把 peer sync 写成“谁最后写入谁生效”的配置中心。

## StorageNode

已确认入口：

- `apps/storage_node_app.cpp`
- `modules/store/node/storage_node_registry.h`
- `modules/store/node/storage_node_registry.cpp`
- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `tests/storage_heartbeat_registry_test.cpp`
- `tests/storage_node_service_test.cpp`
- `tests/storage_node_client_test.cpp`

职责：

- `storage_node_app.cpp` 当前负责 load/create storage identity、启动 chunk store 和 storage RPC、向 ViewNode 注册并持续 heartbeat。
- `storage_node_registry.*` 当前负责本地 data-plane facts registry、TTL/liveness、sequence 防 stale、health/capacity/load partial merge。
- `storage_node_service.*` 暴露 chunk data-plane RPC 和 storage registry fact update RPC。
- `storage_node_client.*` 负责 storage RPC client、fact update RPC、读写/修复/scrub/delete 传输调用。
- `tests/storage_heartbeat_registry_test.cpp` 当前已覆盖 register、heartbeat、partial facts merge、TTL、stale/duplicate update。

输入：

- 本地 `node_id`、endpoint、data dir、capacity、failure domain。
- 向 ViewNode register/heartbeat 时的 health、disk pressure、capacity、load、读写过载状态。
- storage RPC 的 chunk payload / checksum / metadata。

输出：

- 本地 storage service。
- 发送给 ViewNode 的 storage observation。
- 本地 registry snapshot 与 fact update 结果。

边界：

- StorageNode dynamic join 是 discovery-only。
- StorageNode 注册不进入 Raft log。
- StorageNode 注册不影响 quorum、election、metadata committed manifest。
- 新 StorageNode 只参与后续新对象 placement。
- 009 不做旧对象 rebalance。
- `storage_node_app.cpp` 当前确实已经把 StorageNode register/heartbeat 发给 ViewNode，但这只是让 ViewNode 看到 data-plane facts，不是 metadata membership change。
- StorageNode heartbeat 状态边界必须包含 `node_id`、incarnation、sequence、endpoint、capacity、load、health、disk pressure、writable status 等信息。当前入口文件已承载 endpoint、capacity、load、health、disk pressure、写入过载与只读/不可用健康表达；009 扩展时不能丢掉这些字段，也不能把 writable 状态压扁成单一“是否在线”。

容易误用点：

- 把新增 StorageNode 自动解释成要修改旧对象 manifest。
- 把 StorageNode 可发现性误写成 metadata committed object visibility。
- 把 StorageNode heartbeat 当成会触发 Raft membership 或 quorum 变化。

## placement / transfer

已确认入口：

- `modules/store/placement/placement_manager.h`
- `modules/store/placement/placement_manager.cpp`
- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/metadata_transfer_client.cpp`
- `modules/store/transfer/storage_transfer_client.cpp`
- `tests/integrated_object_storage_e2e_test.cpp`

职责：

- `placement_manager.*` 负责基于候选节点做 replica selection；它既能消费本地 `StorageNodeRegistry` snapshot，也能消费 ViewNode-backed snapshot adapter。
- `object_transfer.cpp` 负责 upload/download 会话编排：先经 Metadata 获取 write plan / manifest，再经 ViewNode 发现 metadata/storage endpoint，最后走 storage transfer 和 metadata commit。
- `metadata_transfer_client.cpp` 负责 Metadata RPC、`NOT_LEADER` 重试、CreateWritePlan / CommitObject / GetObjectManifest。
- `storage_transfer_client.cpp` 负责对单个 StorageNode 做 chunk read/write/delete/scrub/repair。
- `tests/integrated_object_storage_e2e_test.cpp` 当前已锁定“manifest 可见性来自 Metadata committed state，不来自 ViewNode 观测或 StorageNode 本地状态”的边界。

输入：

- ViewNode-discovered storage candidates。
- Metadata CreateWritePlan / COMMITTED manifest。
- replica policy、chunk identity、storage target endpoint。

输出：

- 写入 placement decision。
- chunk 传输结果。
- 以 Metadata COMMITTED manifest 为准的上传提交和下载重构结果。

边界：

- placement 应使用 ViewNode-discovered LIVE StorageNode candidates。
- dynamic StorageNode 只影响未来 write plan。
- 不要求旧对象自动迁移。
- 不允许为了使用新 StorageNode 修改已提交 manifest。
- `placement_manager.cpp` 当前已经明确只接受 live 且 facts 完整的 candidate；ViewNode 只提供 observation，最终 placement policy 仍由 placement 模块执行。
- `object_transfer.cpp` 当前上传路径是“Metadata CreateWritePlan -> ViewNode discover storage -> write chunks -> Metadata CommitObject”；下载路径是“Metadata GetObjectManifest -> ViewNode resolve readable replica endpoint -> read chunks -> checksum verify”。因此 ViewNode 只能帮助找 LIVE endpoint，不能替代 COMMITTED manifest authority。

容易误用点：

- 让 ViewNode 直接决定对象副本清单或提交可见性。
- 为了把新 StorageNode 用起来而回写旧 manifest。
- 把 storage discovery 结果当成无需 Metadata 确认的最终 placement authority。

## Metadata service / Raft membership

已确认入口：

- `modules/raft/service/metadata_service_impl.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `modules/raft/replication/replicator.h`
- `modules/raft/replication/replicator.cpp`
- `apps/metadata_node_app.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `tests/metadata_failover_test.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `tests/test_raft_election.cpp`
- `tests/test_raft_log_replication.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_snapshot_restart.cpp`

职责：

- `metadata_service_impl.cpp` 当前负责 metadata leader admission、对象/桶 RPC、`NOT_LEADER` 返回和 committed membership quorum diagnostics。
- `raft_node.*` 当前负责 voter 集合上的选举、AppendEntries、InstallSnapshot、commit/apply、snapshot/restart recovery，以及只读的 committed membership quorum summary。
- `replicator.*` 当前负责对单个已配置 peer 的 AppendEntries / InstallSnapshot 复制状态机。
- `metadata_node_app.cpp` 当前负责固定 `node_id + raft_id` 的身份校验、初始 voter/learner 角色与 config 对齐、启动 RaftNode、向 ViewNode 上报 observed metadata facts。
- 相关 tests 当前覆盖 leader election、log replication、snapshot catch-up、restart recovery、failover、quorum 边界，以及“ViewNode observed voter 不能扩张 committed voter 集合”。

输入：

- Metadata client RPC。
- `NodeConfig` 中的本地 `raft_id` 和配置 peers。
- AppendEntries / RequestVote / InstallSnapshot RPC。
- ViewNode registration/heartbeat 所需的 observed metadata facts。

输出：

- Metadata leader hint / `NOT_LEADER`。
- committed voter quorum diagnostics。
- Raft log replication、snapshot install、restart recovery。
- 对 ViewNode 的 observed metadata registration。

边界：

- MetadataNode dynamic join 是 consensus membership join，不是普通 discovery join。
- 新 MetadataNode 先注册到 ViewNode，只是 observed metadata facts。
- 真正 membership authority 是 Metadata leader + committed Raft config log。
- ViewNode 不能绕过 Raft membership log 决定 voter / learner。
- 新节点先成为 learner / non-voter；learner 可以接收 AppendEntries / InstallSnapshot，可以推进 `match_index` / `applied_index` / `commit_index`，但不参与 quorum、不投票、不能成为 leader。
- committed voter count 必须保持奇数。
- `3 voters + 1 learner` 允许；单 learner ready 后不能 promote 成 `4 voters`，只能进入 pending / waiting-for-pair 一类状态。
- `3 voters + 2 ready learners` 才能通过安全 batch promote / joint consensus / batched membership change 进入 `5 voters`。

当前代码现状与后续入口边界：

- `metadata_service_impl.cpp` 当前没有确认到 dynamic join / add-learner / batch-promote RPC；现有入口主要是 leader authority 和 metadata 业务 RPC。009 的 learner join 需要扩展这里，但不能把普通 `RegisterNode` 伪装成 join。
- `RaftNode::GetCommittedMembershipQuorumSummary()` 当前把 committed voter 集合只读地推导为 `self + config_.peers`，`learner_ids` 仍为空；这说明现阶段 learner 还没有真正下沉到运行时 membership authority。
- `Replicator` 当前已是 AppendEntries / InstallSnapshot 的单 peer 复制入口；009 后续 learner catch-up 也必须沿这个复制/快照入口扩展，而不是走 ViewNode。
- `metadata_node_app.cpp` 当前只接受由 cluster config 固定出来的 Metadata identity，并校验 `initial_role` 必须与初始 membership 一致；它不是 dynamic join candidate 自注册入口。
- `tests/integrated_object_storage_quorum_test.cpp` 已明确验证：额外把 metadata 节点以 observed `VOTER` 注册到 ViewNode，不会扩张真实 committed voter 集合。
- `tests/test_raft_election.cpp`、`tests/test_raft_log_replication.cpp`、`tests/test_raft_snapshot_catchup.cpp`、`tests/test_raft_snapshot_restart.cpp` 当前确认的是现有 voter/follower 复制、快照、恢复入口；009 需要在这些入口附近补 learner 语义，但不能把现有 follower 语义直接偷换成 learner 已完成。

容易误用点：

- 把“向 ViewNode 注册 MetadataNode”写成“已经加入 Raft 集群”。
- 把 local identity 或 cluster config 中的单个 learner 记录写成已经拥有 voter 权限。
- 允许单 learner promote 到 `4 voters`。
- 把 ViewNode leader hint 或 observed membership state 当成 committed membership authority。
