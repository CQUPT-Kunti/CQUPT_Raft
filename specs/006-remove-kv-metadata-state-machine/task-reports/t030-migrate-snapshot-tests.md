# T030 迁移 snapshot 相关 Raft 回归测试到 metadata 路径

## 范围说明

- 本次迁移了 3 个代表性 snapshot 回归测试：
  - `snapshot_test.cpp`
  - `test_raft_snapshot_catchup.cpp`
  - `test_raft_snapshot_restart.cpp`
- 未删除 `KvStateMachine` 和旧 KV Command
- 未修改 `RaftNode` 默认 wiring
- 未进入 `T031`

## 本次迁移的测试

1. `RaftSnapshotRecoveryTest.SavesSnapshotAndRestoresAfterRestart`
2. `RaftSnapshotCatchupTest.FollowerContinuesReplicatingLogsAfterInstallingSnapshot`
3. `RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart`

## 原 KV 依赖

- `CommandType::kSet / kDelete`
- `leader->Propose(Command)`
- `DebugGetValue()`
- 通过 KV key/value 可见性验证 snapshot、restart、catch-up

## 新的 metadata 路径

- 写入改为 `CreateBucket / CreateObject / CommitObject / DeleteObject`
- proposal 改为 `ProposeMetadataCommandWithRetry()`
- 状态验证复用：
  - `WaitUntilAllMetadataRecoveryMatches()`
  - `WaitUntilAllCommittedObject()`
  - `WaitUntilAllDeletedObjectHidden()`
  - `WaitUntilAllListObjectsMatch()`

## 新 metadata 断言

### `snapshot_test.cpp`

- bucket 创建后写入 25 个 committed object
- 额外写入并删除 `deleted_anchor`
- snapshot 生成前先验证：
  - `HeadObject` committed 可见
  - `deleted_anchor` 不暴露
  - `ObjectRecord(deleted_anchor)` 为 `DELETED`
  - `ListObjects / object_index` 一致
  - `ChunkRef` 可查询
  - `request_table` 计数为 54
  - `tombstone` 计数为 1
- full restart 后再次验证同样的 metadata 恢复事实
- restart 后继续提交 `after_restart`，验证 snapshot 恢复后仍可继续复制

### `test_raft_snapshot_catchup.cpp`

- follower 停机后，leader 侧提交 committed baseline objects，并写入/删除 `deleted_anchor`
- 等 leader 产生 snapshot 后重启 follower
- 在 restarted follower 上验证：
  - committed object 可恢复
  - deleted object 不复活
  - `object_index / ListObjects` 一致
  - `ChunkRef` 可恢复
  - `request_table` 计数为 44
  - `tombstone` 计数为 1
  - `LastAppliedIndex` 追到删除边界
- snapshot 安装完成后继续提交 `after_snapshot`，验证 follower 还能继续复制日志

### `test_raft_snapshot_restart.cpp`

- 先写入 snapshot baseline objects，等待 leader 产生 snapshot
- 再提交 tail committed object，并提交/删除 `tail_delete`
- full restart 后验证：
  - snapshot 覆盖数据可恢复
  - tail committed object 可恢复
  - deleted object 不复活
  - `object_index / ListObjects` 一致
  - `ChunkRef` 可恢复
  - `request_table` 计数为 42
  - `tombstone` 计数为 1
  - `LastAppliedIndex` 覆盖到 tail replay 边界
- restart 后再提交 `after_tail_restart`，验证 snapshot + tail replay 后仍能继续写入

## `last_applied_term` 风险

- 本次迁移中，3 个测试都只能把 `MetadataStateMachine::LastAppliedTerm()` 验证为当前实现事实 `0`
- 这不能说明真实 Raft term 已完整恢复
- 该点仍是 metadata snapshot/recovery 的已知实现风险，需后续任务继续处理

## Linux 验证

- Build:
  - `cmake --build --preset debug-ninja-low-parallel --target snapshot_test test_raft_snapshot_catchup test_raft_snapshot_restart`
  - 结果：PASS

- CTest:
  - `ctest --test-dir build/linux --output-on-failure -R '^(RaftSnapshotRecoveryTest\.SavesSnapshotAndRestoresAfterRestart|RaftSnapshotCatchupTest\.FollowerContinuesReplicatingLogsAfterInstallingSnapshot|RaftSnapshotRestartTest\.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart)$'`
  - 结果：PASS
  - 耗时：45s
  - 完整日志：`tmp/test-logs/t030-snapshot-ctest.log`

## 剩余未迁移项

- `test_raft_snapshot_catchup.cpp`
  - `RestartedFollowerCatchesUpLargeGapWithBatchedAppendEntries`
  - `LaggingFollowerReplaysLiveLogWithoutBreakingCommittedDeleteOrdering`
  - `RestartedFollowerInstallsSnapshotWhenLeaderCompactedLogs`
- `test_raft_snapshot_restart.cpp`
  - `FollowerKeepsStateAfterInstallSnapshotAndRestart`
  - `LeaderKeepsCompactedSnapshotStateAfterRestart`
  - `FullClusterRestartsAfterSnapshotAndContinuesWriting`
  - `FullRestartReplaysSnapshotTailWithoutLosingDeletesOrOverwrites`
  - `RestartedFollowerAppliesCommittedTailExactlyOnceAfterSnapshotLoad`
  - standalone corrupted snapshot / publish failure / metadata mismatch recovery tests

## 验收结论

- 已在 3 个 snapshot 相关测试文件中迁移出一组代表性 KV-based Raft 回归测试
- 被迁移测试不再依赖 `CommandType::kSet / kDelete`、`DebugGetValue()`、KV 可见性断言
- 被迁移测试改为通过 `MetadataCommand / MetadataStateMachine` 验证 snapshot、restart、catch-up
- 恢复场景已覆盖 `request_table`、`tombstone`、`object_index`、`chunk_ref_index`、`LastAppliedIndex` 等相关事实
- `LastAppliedTerm` 当前仍只能验证为 `0`，已明确记录为实现风险
