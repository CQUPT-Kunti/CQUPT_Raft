# T057 Test Dedup And Legacy KV Cleanup

## 本次修改文件
- `tests/test_command.cpp`
- `tests/CMakeLists.txt`
- `tests/README.md`
- `tests/metadata_state_machine_test.cpp`
- `tests/support/raft_snapshot_restart_test_utils.h`
- `tests/test_raft_snapshot_restart.cpp`
- `tests/test_raft_snapshot_recovery.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_election.cpp`
- `tests/snapshot_test.cpp`
- `tests/persistence_test.cpp`
- `tests/test_raft_commit_apply.cpp`
- `tests/raft_integration_test.cpp`
- 删除 `tests/test_state_machine.cpp`
- 更新 `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`

## 删除 / 退役的旧 KV 测试
- 退役 `tests/test_state_machine.cpp`
- 从 `tests/CMakeLists.txt` 移除 `test_state_machine` target
- 从 `tests/README.md` 移除 `test_state_machine` / `KvStateMachineTest` 的主入口表述

## 迁移为 metadata-only 的测试
- `tests/test_command.cpp`
  - 不再测试 `SET|` / `DEL|`
  - 改为测试 metadata-only `META|size|payload` 包装与反序列化拒绝路径
- `tests/test_raft_election.cpp`
  - follower reject case 不再构造 `kSet`
  - 改为构造合法 metadata payload，并验证 follower 返回 `NotLeader`
- `tests/test_raft_snapshot_restart.cpp`
- `tests/test_raft_snapshot_recovery.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
  - 旧 `SetCommand/DeleteCommand` synthetic bridge 全部改为 metadata-only synthetic object helper
  - 断言继续落在 metadata state consistency，而不是 KV 可见性接口

## 合并 / 删除的重复 helper 与重复 case
- `tests/support/raft_snapshot_restart_test_utils.h`
  - `SyntheticMetadataCommand` 重命名为 `SyntheticMetadataMutation`
  - `SetCommand/DeleteCommand` 改为 `WriteSyntheticObject/DeleteSyntheticObject`
  - `WaitForValueOnNode/All` 改为 `WaitForSyntheticObjectOnNode/All`
  - `WaitForMissingOnAll` 改为 `WaitForSyntheticObjectMissingOnAll`
  - `WriteManyValues` 改为 `WriteSyntheticObjects`
- `tests/test_raft_snapshot_catchup.cpp`
  - 删除本地重复的 `SetCommand/DeleteCommand`
  - 删除对 `WaitForSingleLeader/FindNodeIndex/PickFollowerIndex/WaitFor* / WriteManyValues` 的一层透传 wrapper
  - 直接复用 `tests/support/raft_snapshot_restart_test_utils.h`
- `tests/snapshot_test.cpp`
  - 删除未再使用的旧 KV helper：`WaitForValueOnAllNodes`、`MakeSet`
- `tests/persistence_test.cpp`
  - 删除未再使用的旧 KV helper：`ProposeSetToLeader`、`ProposeSetWithRetry`、`WaitUntilValue`

## CMake 测试 target 清理
- `tests/CMakeLists.txt` 已移除 `test_state_machine`
- 其余 metadata-only target 名称保持不变

## 保留的 legacy 测试及理由
- `metadata_state_machine_legacy_test.cpp` 保留
- 原因：它验证 metadata 状态机的 legacy metadata 兼容边界，不是 KV fallback

## Linux 结果
- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_command test_metadata_state_machine test_raft_election test_raft_commit_apply raft_integration_test persistence_test snapshot_test test_raft_snapshot_catchup test_raft_snapshot_restart no_kv_surface_audit`：PASS

## Windows 结果
- Windows 未执行，原因是当前环境为 Linux；T057 的 Windows 覆盖将在后续最终复验中确认

## CTest 结果
- 执行：
  - `ctest --test-dir build/linux --output-on-failure -R "(CommandTest|MetadataStateMachine|RaftElectionTest|RaftCommitApply|RaftIntegrationTest|PersistenceTest|RaftSnapshotRecoveryTest|RaftSnapshotRestartTest|RaftSnapshotCatchupTest|NoKvSurfaceAudit)"`
- 结果：
  - 首轮并行运行 `98/100 PASS`
  - 失败项为：
    - `RaftSnapshotRecoveryTest.StandaloneRestartRejectsMetadataMismatchedVisibleSnapshotAndKeepsTrustedBoundary`
    - `RaftSnapshotRecoveryTest.AllPublishedSnapshotsInvalidYieldNoTrustedSnapshot`
  - 失败现象是 leader 长时间不稳定，日志更像并发运行互扰，不是 metadata 断言回归
- 最小复验：
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "(RaftSnapshotRecoveryTest\\.StandaloneRestartRejectsMetadataMismatchedVisibleSnapshotAndKeepsTrustedBoundary|RaftSnapshotRecoveryTest\\.AllPublishedSnapshotsInvalidYieldNoTrustedSnapshot)"`
  - 结果：`2/2 PASS`
- `NoKvSurfaceAudit`：PASS

## tests 侧 KV residual status
- tests 业务代码 / helper 已不再引用已删除的生产 KV 符号：
  - `CommandType::kSet`
  - `CommandType::kDelete`
  - `DebugGetValue()`
  - `raft/state_machine/state_machine.h`
  - `KvStateMachine`
- 当前剩余 KV 相关字符串主要在：
  - `tests/AGENTS.md` 的历史职责说明
  - `tests/no_kv_surface_audit.cmake` 的自审计关键词
- 这些属于文档 / 审计规则残留，不是业务测试逻辑残留

## 是否降低覆盖
- 否
- 退役的是纯 `KvStateMachine` 单测
- snapshot / restart / catch-up / recovery / election / integration 的高价值回归均保留，并迁成 metadata-only 断言路径

## 是否修改生产代码
- 否

## 是否进入 T058
- 可以进入 `T058`

## 剩余风险
- `tests/AGENTS.md` 仍含 `test_state_machine.cpp` 的历史说明；按任务边界未修改
- recovery/snapshot 组在并行 `ctest` 下存在 leader 竞争互扰，已记录到 `cross-task-risk-notes.md`
- `T058` 需要把 `tests/no_kv_surface_audit.cmake` 里的旧 blocker 表述升级到新的严格状态
- `T059` 需要继续修正脚本入口的 no-KV / recovery 低并发执行策略
