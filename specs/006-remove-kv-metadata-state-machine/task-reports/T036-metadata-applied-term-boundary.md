## T036 MetadataStateMachine applied term 边界修复

### 修改文件
- `modules/raft/state_machine/state_machine_interface.h`
- `modules/raft/state_machine/state_machine.h`
- `modules/raft/state_machine/state_machine.cpp`
- `modules/raft/state_machine/metadata_state_machine.h`
- `modules/raft/state_machine/metadata_state_machine.cpp`
- `modules/raft/node/raft_node.cpp`
- `tests/support/metadata_test_utils.h`
- `tests/metadata_raft_test_utils.h`
- `tests/metadata_state_machine_test.cpp`
- `tests/persistence_test.cpp`
- `tests/raft_integration_test.cpp`
- `tests/snapshot_test.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_snapshot_restart.cpp`

### applied term 边界如何修复
- `IStateMachine` 新增三参 apply 入口：`Apply(index, term, command_data)`。
- 原两参 `Apply(index, command_data)` 保留为兼容转发，默认传 `term=0`，避免无关旧调用点大范围重写。
- `RaftNode::ApplyCommittedEntries()` 现在从 `LogRecord` 读取真实 `record->term`，并在 apply 时传给状态机。
- `MetadataStateMachine` 在以下成功路径统一推进真实边界：
  - internal noop
  - `CreateBucket`
  - `DeleteBucket`
  - `CreateObject`
  - `CommitObject`
  - `AbortObject`
  - `DeleteObject`
- duplicate `request_id` 命中 idempotent replay 时，仍直接返回 replay success，不重复推进 `last_applied_index / last_applied_term`。

### snapshot save/load
- `MetadataStateMachine::SaveSnapshot()` 继续写出 `last_applied_index` 与 `last_applied_term`。
- 这次修复的关键不是格式新增字段，而是把运行态真实 term 写进去，不再把成功 apply 后的 term 固定成 `0`。
- `LoadSnapshot()` 继续恢复 `last_applied_index / last_applied_term`，现在恢复的值是真实 term。

### 真实 term 传递点
- `RaftNode::ApplyCommittedEntries()`：
  - 读取 `record->index`
  - 读取 `record->term`
  - 调用 `state_machine->Apply(apply_index, apply_term, command_data)`
- `CompositeKvMetadataStateMachine`、`KvStateMachine`、`StrongConsistencyMetadataStateMachine` 同步适配三参接口；其中 KV / metadata V1 仅忽略 term，不改旧语义。

### 测试更新
- `MetadataStateMachineTest`
  - 成功 apply 的 bucket/object/commit/abort/delete 用例改为验证非零真实 term
  - 新增 `DuplicateRequestReplayDoesNotAdvanceAppliedTerm`
  - 新增 `SaveSnapshotAndLoadSnapshotPreserveAppliedTerm`
- `PersistenceTest`
  - `FullClusterRestartRecovery`
  - `RestartedFollowerCatchesUp`
  - `ColdRestartPreservesPersistedHardStateBeforeStart`
  - 都不再接受 `LastAppliedTerm()==0`
- `RaftIntegrationTest.LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary`
  - 改为验证恢复后 applied term 至少达到最后 metadata entry term
- `RaftSnapshotRecoveryTest.SavesSnapshotAndRestoresAfterRestart`
- `RaftSnapshotCatchupTest.FollowerContinuesReplicatingLogsAfterInstallingSnapshot`
- `RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart`
  - 都改为验证恢复后的非零真实 term 边界
- `tests/metadata_raft_test_utils.h`
  - `MetadataRecoveryExpectation` 新增 `expected_min_last_applied_term`
  - 用于恢复类测试验证 term 下界，不再把 `0` 当通过

### Linux 验证
- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine persistence_test raft_integration_test snapshot_test test_raft_snapshot_catchup test_raft_snapshot_restart test_raft_log_replication test_raft_commit_apply test_t017_leader_switch_ordering test_metadata_failover`：PASS
- 主验证：
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "(MetadataStateMachineTest|PersistenceTest\\.(FullClusterRestartRecovery|RestartedFollowerCatchesUp|ColdRestartPreservesPersistedHardStateBeforeStart)|RaftIntegrationTest\\.LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary|RaftSnapshotRecoveryTest\\.SavesSnapshotAndRestoresAfterRestart|RaftSnapshotCatchupTest\\.FollowerContinuesReplicatingLogsAfterInstallingSnapshot|RaftSnapshotRestartTest\\.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart|RaftLogReplicationTest|RaftCommitApplyTest|LeaderSwitchOrderingTest|MetadataFailoverTest)"`：PASS
- CTest 结果：`54/54` 通过

### Windows 结果
- Windows 未执行，原因是当前环境为 Linux；T036 的 Windows 覆盖将在后续 Windows 验证阶段统一执行

### 覆盖范围与未跑全量原因
- 没有跑全量 CTest。
- 本轮接口改动落在状态机 apply 边界，因此验证集中在：
  - metadata state machine 单测
  - 已迁移 metadata 路径的 persistence / integration / snapshot / catch-up / restart 用例
  - 一小组 `RaftLogReplication` / `RaftCommitApply` / `LeaderSwitchOrdering` / `MetadataFailover` 代表性回归
- 旧的非 T036 范围 persistence/snapshot 子用例没有纳入最终验收过滤。

### 剩余风险
- 两参 `Apply(index, command_data)` 兼容入口仍存在，默认 term 仍是 `0`；这保证了旧测试/旧状态机调用兼容，但也意味着未来若新增 metadata 状态机测试，必须优先走三参 apply 或 `RaftNode` 真正的 committed apply 路径。
- 对 full-cluster restart 后的新 leader no-op，恢复类测试采用“`LastAppliedTerm >= 最后 metadata entry term`”而不是硬编码“必须等于某个固定 term”，因为 restart 后可能合法出现更高 term 的 no-op apply；这不是占位事实，而是当前 Raft 时序下的真实边界。
- 本轮没有改 `RaftNode` 默认 wiring、没有删 KV、没有进入 T037。
