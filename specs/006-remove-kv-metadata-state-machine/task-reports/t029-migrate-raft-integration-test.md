# T029 迁移 raft_integration_test.cpp 到 metadata 路径

## 范围说明

- 本次只处理 `tests/raft_integration_test.cpp`
- 复用并扩展了 `tests/metadata_raft_test_utils.h`
- 未删除 `KvStateMachine`
- 未删除旧 KV Command
- 未修改 `RaftNode` 默认 wiring
- 未进入 `T030`

## 本次迁移的代表性集成测试

1. `RaftIntegrationTest.ReplicatesSetAndDeleteCommandsToAllNodes`
2. `RaftIntegrationTest.ElectsNewLeaderAfterCurrentLeaderStops`
3. `RaftIntegrationTest.GeneratesSnapshotMetaFileAfterEnoughAppliedLogs`
4. `RaftIntegrationTest.LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary`

`RaftIntegrationTest.ElectsSingleLeaderInThreeNodeCluster` 本身不依赖 KV 业务断言，保持不变。

## 原 KV 依赖

- `CommandType::kSet`
- `CommandType::kDelete`
- `leader->Propose(Command)`
- `DebugGetValue()`
- 通过 KV key/value 可见性验证复制、failover、snapshot、catch-up

## 本次 metadata 迁移内容

### helper

- 在 `tests/metadata_raft_test_utils.h` 新增 `ProposeMetadataCommandWithRetry()`
- 继续复用：
  - `WaitUntilAllCommittedObject()`
  - `WaitUntilAllDeletedObjectHidden()`
  - `WaitUntilAllListObjectsMatch()`
  - `WaitUntilAllMetadataRecoveryMatches()`

### `ReplicatesSetAndDeleteCommandsToAllNodes`

- 改为 `CreateBucket -> CreateObject(x) -> CommitObject(x) -> CreateObject(y) -> CommitObject(y) -> DeleteObject(y)`
- 不再验证 KV 值
- 改为验证：
  - `HeadObject(x)` committed 可见
  - `DeleteObject(y)` 后 `HeadObject(y)` 不可见
  - `ObjectRecord(y)` 进入 `DELETED`
  - `ListObjects(bucket)` 与 `object_index` 一致，只剩 `x`
  - `ChunkRef(x)` 可查询
  - `LastAppliedIndex` 至少推进到删除请求

### `ElectsNewLeaderAfterCurrentLeaderStops`

- 原来是 failover 后 `SET after_failover=ok`
- 改为 failover 后 `CreateBucket + CreateObject(after_failover) + CommitObject(after_failover)`
- 断言改为 surviving nodes 的 metadata 状态收敛：
  - committed object 可见
  - `object_index/ListObjects` 一致
  - `request_table` 计数为 3
  - `tombstone` 计数为 0
  - `LastAppliedIndex` 推进到 commit 边界

### `GeneratesSnapshotMetaFileAfterEnoughAppliedLogs`

- 原来用 8 次 `SET snap_i=value_i` 触发 snapshot
- 改为：
  - `CreateBucket`
  - 对 8 个对象执行 `CreateObject + CommitObject`
- 除了保留 `__raft_snapshot_meta` 生成检查，还新增：
  - `ListObjects(prefix=snap_)` 与 `object_index` 一致
  - 所有 committed object 在各节点收敛

### `LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary`

- 原来是：
  - follower 停机
  - `SET boundary_key`
  - 多次 `SET boundary_fill_i`
  - `DEL boundary_key`
  - `SET boundary_tail`
- 改为：
  - follower 停机
  - `CreateBucket`
  - `CreateObject/CommitObject(boundary_key)`
  - 8 个 filler object 的 `CreateObject/CommitObject`
  - `DeleteObject(boundary_key)`
  - `CreateObject/CommitObject(boundary_tail)`
- 在恢复场景中不只看对象可见性，新增验证：
  - `request_table` 计数恢复为 22
  - `tombstone` 计数恢复为 1
  - `object_index/ListObjects` 与预期 committed keys 一致
  - deleted object `boundary_key` 不复活
  - committed tail object `boundary_tail` 可恢复
  - restarted follower 的 `LastAppliedIndex` 追到 tail commit
  - restarted follower 的 `LastAppliedTerm` 当前实现事实仍为 `0`

## 仍待迁移 / 后续处理项

- `snapshot_test.cpp` 仍存在 KV-based snapshot/save/load/replay 断言
- `test_raft_snapshot_catchup.cpp` 仍存在 KV-based catch-up 断言
- `test_raft_snapshot_restart.cpp` 仍存在 KV-based snapshot/restart 断言
- `persistence_test.cpp` 中其余 trusted-boundary / failure-injection 用例仍有 KV payload
- `raft_integration_test.cpp` 中无新的 KV demo-only 用例需要单独删除；本次保留的测试都属于 Raft 集成行为验证

## Linux 验证

- Build:
  - `cmake --build --preset debug-ninja-low-parallel --target raft_integration_test`
  - 结果：PASS

- 定向 CTest:
  - `ctest --test-dir build/linux --output-on-failure -R '^RaftIntegrationTest\.'`
  - 结果：PASS
  - 耗时：16s
  - 完整日志：`tmp/test-logs/t029-raft-integration-ctest.log`

- helper 代表性回归补跑：
  - `cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication test_raft_commit_apply test_t017_leader_switch_ordering persistence_test`
  - `ctest --test-dir build/linux --output-on-failure -R '^(RaftLogReplicationTest|RaftCommitApplyTest|RaftLeaderSwitchOrderingTest|PersistenceTest\.(FullClusterRestartRecovery|RestartedFollowerCatchesUp|ColdRestartPreservesPersistedHardStateBeforeStart))'`
  - 结果：PASS
  - 耗时：52s
  - 完整日志：`tmp/test-logs/t029-helper-regression-ctest.log`

## Windows

- 本次未验证
- 不声明 Windows PASS

## 验收结论

- `raft_integration_test.cpp` 中一组有代表性的 KV-based 集成测试已迁移到 metadata 路径
- 被迁移测试不再依赖 KV Put/Get/Delete、`CommandType::kSet/kDelete`、`DebugGetValue()`
- 被迁移测试改为使用 `MetadataCommand / MetadataStateMachine` 验证 leader election、replication、failover、snapshot、lagging follower catch-up
- 恢复场景已覆盖 `request_table`、`tombstone`、`object_index`、`chunk_ref`、`LastAppliedIndex` 等相关事实
- `LastAppliedTerm` 当前只能验证“实现事实为 0”，未在本次通过改实现解决
