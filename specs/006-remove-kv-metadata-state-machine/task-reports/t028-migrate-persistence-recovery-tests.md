# T028 迁移 persistence / restart recovery 回归测试到 metadata 路径

## 范围说明

- 本次按用户当前执行项处理 `persistence_test.cpp` 中与 restart/recovery 强相关的用例。
- 未修改 `KvStateMachine`、旧 KV Command、`RaftNode` 默认 wiring、持久化格式、协议语义。
- 未进入 `T029`。

## 本次迁移的测试

1. `PersistenceTest.FullClusterRestartRecovery`
2. `PersistenceTest.RestartedFollowerCatchesUp`
3. `PersistenceTest.ColdRestartPreservesPersistedHardStateBeforeStart`

## 原 KV 依赖

- 通过 `CommandType::kSet`
- 通过 `DebugGetValue()`
- 通过 KV key/value 可见性验证 restart / recovery

## 迁移后的 metadata 路径

- 写入命令改为 `CreateBucket`、`CreateObject`、`CommitObject`、`DeleteObject`
- Raft proposal 统一走 `ProposeMetadata()`
- 断言统一走 `MetadataStateMachine` / `metadata_raft_test_utils.h`

## 新增 / 收拢的 metadata 恢复断言

- 在 `tests/metadata_raft_test_utils.h` 新增 `MetadataRecoveryExpectation`
- 在 `tests/metadata_raft_test_utils.h` 新增 `WaitUntilAllMetadataRecoveryMatches()`
- 统一验证：
  - bucket 仍为 active
  - committed object 的 `HeadObject` 可恢复
  - deleted object 的 `HeadObject` 仍为 `NotFound`
  - `FindObject` 中 deleted object 保留 tombstone 对应删除事实
  - `FindIndexedObjectId` 与 `ListObjects` 对 committed / deleted 状态一致
  - `FindChunkRefs` 对 committed object 可恢复，对 deleted object 不残留
  - `RequestCount()` / `TombstoneCount()` 恢复一致
  - `LastAppliedIndex()` 恢复一致或达到预期边界

## 各测试覆盖的恢复事实

### `FullClusterRestartRecovery`

- 三节点全停再启动后恢复 metadata 状态
- committed object `alpha` 可恢复
- deleted object `gone` 不复活
- `object_table` / `object_index` / `chunk_ref_index` 一致
- `request_table` 计数恢复为 6
- `tombstone` 计数恢复为 1

### `RestartedFollowerCatchesUp`

- follower 停机期间，leader/另一 follower 继续提交 metadata 写入
- follower 重启后追赶到：
  - committed object `first`
  - committed object `second`
  - deleted object `gone`
- 重启 follower 的 `object_index` / `chunk_ref_index` / tombstone 状态与存活节点收敛一致
- `request_table` 计数恢复为 8
- `tombstone` 计数恢复为 1
- `LastAppliedIndex()` 追赶到删除请求对应边界

### `ColdRestartPreservesPersistedHardStateBeforeStart`

- 单节点 stop 后直接构造新 `RaftNode`，在 `Start()` 前验证 persisted hard state 与 metadata state 一致
- committed object `alpha` 可恢复
- deleted object `gone` 不复活
- `object_table` / `object_index` / `chunk_ref_index` / tombstone 一致
- `request_table` 恢复为 6，`tombstone` 恢复为 1
- `LastAppliedIndex()` 在冷启动恢复后保持与 stop 前一致
- 通过恢复后的 metadata snapshot clone 继续验证：
  - 相同 `request_id` + 相同 fingerprint => `idempotent replay`
  - 相同 `request_id` + 不同 fingerprint => `idempotency conflict`
  - replay 后 deleted object 仍不复活

## `last_applied_term` 当前可验证事实与风险

- 本次迁移中，`MetadataStateMachine::LastAppliedTerm()` 在 apply、cold restart、follower catch-up、snapshot clone 后稳定为 `0`
- 迁移后的测试已固定这一“当前实现事实”，并验证 restart 前后该值保持一致
- 风险：
  - 当前无法把 `LastAppliedTerm()` 断言为真实 Raft log term
  - 这说明 metadata V2 的 applied-term 边界仍需后续在 snapshot/recovery 主任务中补强
  - 该风险未在本次通过“改实现”解决，留给后续 US4/T036/T039 继续处理

## 未迁移 / 后续处理项

- `persistence_test.cpp` 中其余 cold-restart trusted-boundary / publish-failure 用例仍使用 KV payload
- `raft_integration_test.cpp` 仍存在 KV-based recovery/assert 路径
- `snapshot_test.cpp` 仍存在 KV-based snapshot/save/load/replay 断言
- `test_raft_snapshot_catchup.cpp` 仍存在 KV-based catch-up 断言
- `test_raft_snapshot_restart.cpp` 仍存在 KV-based snapshot/restart 断言

## Linux 验证

- Configure:
  - `cmake --preset debug-ninja-low-parallel`
  - 结果：PASS
- Build:
  - `cmake --build --preset debug-ninja-low-parallel --target persistence_test`
  - 结果：PASS
- CTest:
  - `ctest --test-dir build/linux --output-on-failure -R 'PersistenceTest\.(FullClusterRestartRecovery|RestartedFollowerCatchesUp|ColdRestartPreservesPersistedHardStateBeforeStart)$'`
  - 结果：PASS
  - 耗时：11s
  - 完整日志：`tmp/test-logs/t028-persistence-ctest.log`

## Windows

- 本次未验证
- 不声明 Windows PASS

## 验收结论

- 已迁移一组 persistence / restart recovery 相关 KV-based Raft 回归测试到 metadata 路径
- 被迁移测试不再依赖 KV Put/Get/Delete、`CommandType::kSet/kDelete`、`DebugGetValue()`、`kv=...`
- 已覆盖 request_table / request_fingerprints、tombstone、object_index、chunk_ref_index、deleted object 不复活、committed object 可恢复 等当前场景关键事实
- `LastAppliedTerm()` 仅能固定当前实现事实 `0`，已明确登记为后续风险，不把它伪装成真实 term 恢复已完成
- `KvStateMachine`、旧 KV Command、默认 wiring 均未删除或回退
