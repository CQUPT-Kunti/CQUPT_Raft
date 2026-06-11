# Contract: Metadata Learner Join And Odd Voter Membership

## Scope

本合同定义 Metadata/RaftNode 运行中动态加入、learner catch-up、pending learner、odd voter invariant、batch promote / joint consensus / batched membership change。MetadataNode join 是共识成员变更问题，不能由 ViewNode 决定。

## Baseline Entry Points

- 当前 Metadata service：`modules/raft/service/metadata_service_impl.cpp`
- 当前 Raft node：`modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp`
- 当前 replication：`modules/raft/replication/replicator.h`、`modules/raft/replication/replicator.cpp`
- 当前 Metadata app：`apps/metadata_node_app.cpp`
- 当前 tests：`tests/test_raft_election.cpp`、`tests/test_raft_log_replication.cpp`、`tests/test_raft_commit_apply.cpp`、`tests/test_raft_snapshot_catchup.cpp`、`tests/test_raft_snapshot_restart.cpp`、`tests/integrated_object_storage_quorum_test.cpp`、`tests/metadata_failover_test.cpp`、`tests/metadata_client_scenario_test.cpp`

## Dynamic Join Flow

1. Dynamic MetadataNode 启动。
2. 加载或创建 candidate identity，`membership_state=joining/candidate`。
3. 启动 client RPC 和 raft peer RPC。
4. 向 ViewNode 注册 observed metadata facts。
5. 通过 ViewNode 发现 Metadata leader，并保留处理 `NOT_LEADER` 的重试路径。
6. 向 Metadata leader 发起 JoinMetadataCluster 或等价 join 请求。
7. leader 校验 cluster_id、node_id、endpoint、重复注册、pending membership change。
8. leader 通过 committed Raft config log 接受节点为 learner。
9. learner 接收 AppendEntries / InstallSnapshot，推进 match_index / applied_index / commit_index。
10. catch up 到阈值后进入 ready-to-promote。
11. promote 前检查目标 committed voter count 必须为奇数。
12. 如果单独 promote 会变成 4/6 voters，返回 blocked status，learner 保持 waiting-for-pair 并继续追日志。
13. 两个 ready learners 可通过安全 batch membership change / joint consensus / batched promote 一起进入 5/7 voters。

## Learner Invariants

- learner 不参与 RequestVote 投票。
- learner 不参与 quorum 计算。
- learner 不能成为 leader。
- learner 可以接收 AppendEntries。
- learner 可以接收 InstallSnapshot。
- learner 可以推进 match_index / applied_index / commit_index。
- promote-to-voter 只有在 committed membership change 之后生效。

## Odd Voter Invariant

- Committed voter membership 的 voter 数量必须始终为奇数。
- 3 voters + 1 learner 允许；单 learner ready 也不能 promote 成 4 voters。
- 3 voters + 2 ready learners 可以通过安全批量变更直接成为 5 voters。
- 5 voters + 1 learner 允许；单 learner ready 也不能 promote 成 6 voters。
- 5 voters + 2 ready learners 可以通过安全批量变更直接成为 7 voters。
- 不允许先 committed 4 voters 再 committed 5 voters。
- quorum 始终基于 committed voters：3 voters quorum=2，5 voters quorum=3。

## Failure Semantics

- 同一时间默认只允许一个 pending membership change，除非实现了安全 batch/joint 并发语义。
- leader 在 learner catch-up 或 batch promote 中故障时，新 leader 必须从 committed membership log 恢复或安全中止 pending 流程。
- 重复 join 请求必须幂等或返回明确冲突状态。
- 长期无法 catch up 的 learner 可以保留为 learner、标记 unhealthy，或等待显式 admin 移除；不能自动 promote。

## Validation Requirements

- 3 voters + 1 learner：learner 接收日志/snapshot，quorum 仍为 2，不能投票或成为 leader。
- 单 learner promote 返回 `blocked_by_even_voter_count` / `waiting_for_pair` / `need_another_ready_learner` 等明确状态。
- 3 voters + 2 ready learners：batch promote 后 committed voters=5，quorum=3，且没有 committed 4 voters。
- 当前如不支持 batch promote / joint consensus，任务必须明确记录 learner join/catch-up 已完成，promote-to-voter 受阻于安全批量 membership change。

