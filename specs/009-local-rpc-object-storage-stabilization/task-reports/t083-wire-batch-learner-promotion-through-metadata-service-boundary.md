# T083 实施报告

## 结果

PASS

## 做了什么

- 在 `MetadataServiceImpl::JoinMetadataCluster(...)` 中补齐 batch learner promotion 的 service routing 边界。
- service 现在会先走现有 `ProposeAddLearner(...)` admission 路径；当请求命中现有 pending learner、该 learner 已 ready、且当前已有两个 ready learners 时，再显式路由到 `RaftNode` 的原子 batch promote 边界。
- 将 batch promote 从 `RaftNode` 心跳自动触发收口为由 Metadata service 显式调用安全边界，避免 service 入口和 heartbeat 自动 promote 竞争。
- 响应现在使用 promote 后的最新 committed/runtime membership 诊断，而不是 promote 前的旧快照。

## 修改文件

- `modules/raft/service/metadata_service_impl.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `tests/test_raft_snapshot_restart.cpp`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Metadata service 如何调用 RaftNode batch promote boundary

- 入口仍然是 `JoinMetadataCluster`，没有绕过现有 proto/service 边界。
- service 先调用 `RaftNode::ProposeAddLearner(...)`。
- 如果返回的是 duplicate，并且 runtime membership 表明：
  - 该请求对应的是现有 pending learner；
  - 该 learner 已 ready-to-promote；
  - 当前 ready learners 数量至少为 2；
  则 service 再调用 `RaftNode::PromoteReadyLearnerBatch(...)`。
- `PromoteReadyLearnerBatch(...)` 内部只复用已有原子 batch promotion 日志路径，最终 committed membership 仍只来自 `RaftNode` 的原子日志 apply。

## 如何校验 ready learners

- service 通过 runtime membership summary 查找请求对应的 learner entry。
- 只对“现有 pending learner 且已 ready_to_promote”的 duplicate join 请求开放 promote route。
- learner 仍在 catching_up 时，service 不会触发 promote。
- 只有两个 ready learners 同时存在时，service 才会进入 batch promote 路径。

## 单 learner / no-4-voter / 5-voter 语义

- 单 learner 仍然返回 `waiting_for_pair` 语义，不会 promote，也不会参与 quorum。
- service 不会自己拼 voter set；committed membership 只能通过 `RaftNode` 原子日志一次性变成 `5 voters`。
- 因此过程中不会形成 committed `4-voter membership`。
- promote 成功后的服务响应会反映最新诊断：`committed_voter_count=5`、`committed_quorum_size=3`、`runtime_voter_count=5`、`runtime_learner_count=0`。

## 验证

实际 target 名称与任务建议稍有不同，仓库中的真实 target 为：

- `test_metadata_client_scenario`
- `integrated_object_storage_quorum`
- `test_metadata_failover`
- `test_raft_snapshot_restart`

构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario integrated_object_storage_quorum test_metadata_failover test_raft_snapshot_restart ) 9>/tmp/cqupt_raft_build.lock
```

测试命令：

```bash
ctest --preset debug-tests -R "MetadataClientScenario|IntegratedObjectStorageQuorum|MetadataFailover|Failover|RaftSnapshotRestart|SnapshotRestart" --output-on-failure
```

结果：

- 构建 PASS
- 定向 CTest `43/43 PASS`

日志：

- build：`tmp/test-logs/t083-build.log`
- test：`tmp/test-logs/t083-ctest.log`

## 备注

- 当前 batch promote 的 service contract 仍复用 `JoinMetadataCluster`，没有独立 promote RPC。
- 这保证了最小改动，但 promote 结果更多依赖 `committed_membership_changed` 和 diagnostics message 表达；该残余 contract/diagnostics 风险已同步到 `cross-task-risk-notes.md`。
