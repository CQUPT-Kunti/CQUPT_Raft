# T040 RaftNode snapshot replay boundary

## 修改概览
- 更新 `modules/raft/node/raft_node.cpp` 的 startup snapshot load 与 `OnInstallSnapshot` 路径。
- 新增 `ResolveLoadedSnapshotAppliedBoundary(...)` helper：
  - 对 `MetadataStateMachine` 读取 snapshot 恢复后的 `LastAppliedIndex/LastAppliedTerm`
  - 校验其必须与 snapshot catalog / InstallSnapshot RPC 的 `last_included_index/term` 一致
  - 不一致时明确失败，不继续把不可信边界写回 `RaftNode`
- startup load 成功后：
  - `CompactLogPrefixLocked(...)` 使用已校验的 applied boundary
  - `commit_index_` 至少推进到该 boundary
  - `last_applied_` 直接对齐为 snapshot 恢复出的 boundary
- follower `OnInstallSnapshot` 成功后：
  - 运行时 applied boundary 改为直接对齐 snapshot 恢复出的 boundary
  - 后续 replay 自然从 `last_applied_ + 1` 开始，只会回放 `index > snapshot.last_applied_index`

## replay 边界如何保证
- `ApplyCommittedEntries()` 仍保持从 `last_applied_ + 1` 开始回放。
- 本次修正的关键是保证 startup / install snapshot 之后的 `last_applied_` 来自已恢复 snapshot，而不是仅依赖外层 catalog 元数据的盲写。
- 对 metadata V2 snapshot：
  - 若状态机恢复出的 `LastAppliedIndex/Term` 与 catalog / RPC 边界不一致，直接报错或跳过该 snapshot
  - 因此不会用重复 apply 掩盖边界错误

## 已验证的 metadata 恢复事实
- restart recovery 后：
  - `request_table` / `request_fingerprints` 幂等事实保持有效
  - `tombstone` 保留，deleted object 不复活
  - `object_table` / `object_index` 保持一致
  - `chunk_ref_index` 保持一致，committed object 的 `ChunkRef` 可恢复
  - `last_applied_index` / `last_applied_term` 恢复并继续推进
- follower catch-up 后：
  - follower 与 leader 的 `request_count`、`tombstone_count`、`object_count`、`bucket_count`
  - 以及 `last_applied_index` / `last_applied_term`
  - 能收敛到同一份 metadata 事实

## Linux 验证
- 构建：
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target persistence_test raft_integration_test snapshot_test test_raft_snapshot_catchup test_raft_snapshot_restart test_metadata_recovery_stress`
  - 结果：PASS
- 定向测试：
  - `build/linux/tests/test_metadata_recovery_stress --gtest_filter='MetadataRecoveryStressTest.*'`
  - `build/linux/tests/test_raft_snapshot_restart --gtest_filter='RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart'`
  - `build/linux/tests/snapshot_test --gtest_filter='RaftSnapshotRecoveryTest.SavesSnapshotAndRestoresAfterRestart'`
  - `build/linux/tests/persistence_test --gtest_filter='PersistenceTest.FullClusterRestartRecovery:PersistenceTest.RestartedFollowerCatchesUp:PersistenceTest.ColdRestartPreservesPersistedHardStateBeforeStart'`
  - `build/linux/tests/raft_integration_test --gtest_filter='RaftIntegrationTest.LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary'`
  - `build/linux/tests/test_raft_snapshot_catchup --gtest_filter='RaftSnapshotCatchupTest.FollowerContinuesReplicatingLogsAfterInstallingSnapshot'`
  - 结果：PASS

## 剩余风险
- 目前没有单独注入“metadata snapshot 文件边界与 catalog / RPC 边界不一致”的 install/startup 故障用例；本次通过运行时显式校验保证明确失败。
- 用户建议的广义 `ctest -R "(PersistenceTest|RaftIntegrationTest|RaftSnapshotRecoveryTest|RaftSnapshotCatchupTest|RaftSnapshotRestartTest|MetadataRecoveryStressTest)"` 会碰到现存的非 T040 blocker：
  - 部分旧 snapshot recovery/catch-up 用例仍用 KV `SetCommand` 驱动默认 metadata wiring
  - 失败原因为 `failed to parse metadata command`
  - 本次未处理该 blocker，因为不属于 T040 范围
