# T027 Windows 补测报告

日期：2026-05-22  
范围：仅记录 Windows `Debug` 环境补测结果，不修改源码 / 测试 / CMake / `tasks.md`

## 1. Windows Configure 结果

- 命令：`cmake --preset windows`
- 结果：PASS
- 耗时：10.02s
- 日志：`tmp/test-logs/t027-windows-configure.log`

## 2. Windows Build 结果

- 定向构建命令：`cmake --build --preset windows-debug --target raft_demo raft_metadata_client test_metadata_state_machine test_metadata_client_scenario test_metadata_failover test_raft_log_replication test_raft_commit_apply test_t017_leader_switch_ordering`
- 定向构建结果：PASS
- 定向构建耗时：142.80s
- 定向构建日志：`tmp/test-logs/t027-windows-build.log`

- 为补跑 managed tests 追加执行：`cmake --build --preset windows-debug`
- 全量 Windows Debug 构建结果：PASS
- 全量构建耗时：64.64s
- 全量构建日志：`tmp/test-logs/t027-windows-full-build.log`

## 3. T027 直接相关测试结果

- 命令：`ctest --test-dir build/windows -C Debug --output-on-failure -R "^(RaftLeaderSwitchOrderingTest|RaftLogReplicationTest|RaftCommitApplyTest)\\."`
- 结果：PASS
- 汇总：6/6 通过，0 失败
- 总耗时：22.99s
- 日志：`tmp/test-logs/t027-windows-raft-regression.log`

通过用例：
- `RaftLogReplicationTest.LeaderProposeReplicatesLogToAllNodes`
- `RaftLogReplicationTest.MultipleSequentialEntriesStayConsistentAcrossCluster`
- `RaftCommitApplyTest.CommitAndApplyIndexesAdvanceAfterSuccessfulPropose`
- `RaftCommitApplyTest.DeleteCommandIsAppliedToAllNodes`
- `RaftLeaderSwitchOrderingTest.CommittedStateSurvivesLeaderSwitchAndNewLeaderContinuesReplication`
- `RaftLeaderSwitchOrderingTest.LaggingFollowerCatchesUpDuringLeaderSwitchWithoutCommitApplyReordering`

结论：
- T027 迁移后的 leader switch 测试在 Windows 通过。
- 已迁移的 log replication / commit apply 在 Windows 通过。

## 4. Metadata 主路径测试结果

- 命令：`ctest --test-dir build/windows -C Debug --output-on-failure -R "^(MetadataStateMachineTest|MetadataFailoverTest|MetadataClientScenarioTest)\\."`
- 结果：PASS
- 汇总：40/40 通过，0 失败
- 总耗时：5.16s
- 日志：`tmp/test-logs/t027-windows-metadata-main-path.log`

覆盖结论：
- `MetadataStateMachineTest` 通过，覆盖默认 metadata wiring、bucket/object lifecycle、`request_id` 幂等、`tombstone` 删除事实、`object_index` / `chunk_ref` 相关状态、snapshot round-trip、`last_applied_index` 边界、并发 apply/query 一致性。
- `MetadataFailoverTest` 通过，覆盖 leader failover 后 committed 可见、pending 隐藏、同一 commit request 在新 leader 重试。
- `MetadataClientScenarioTest` 通过，覆盖 metadata client 主路径、create/commit/head/list/delete 流程、重复 `request_id` 去重、chunk layout / etag 可见性。

从本轮 Windows 补测看：
- metadata 路径未观察到回退到 KV fallback。
- `RaftNode` 默认 wiring 的 metadata 主路径在相关测试中保持可用。
- 删除对象不复活，`Head/List` 不暴露 `DELETED` 对象的相关用例在本轮通过。

## 5. Windows Managed Tests 结果

首次执行：
- 命令：`ctest --preset windows-debug-managed-tests`
- 结果：FAIL
- 现象：在仅完成定向构建后，出现 20 个 `*_NOT_BUILT`，属于测试二进制未生成，不是断言失败。
- 日志：`tmp/test-logs/t027-windows-managed-tests.log`

补全构建后再次执行：
- 命令：`ctest --preset windows-debug-managed-tests`
- 结果：FAIL
- 汇总：143/182 通过，39 失败
- 通过率：79%
- 总耗时：425.91s
- 日志：`tmp/test-logs/t027-windows-managed-tests-rerun.log`

## 6. 失败项摘要

失败套件主要集中在尚未迁移完成的旧 Raft/KV 回归路径：
- `RaftSplitBrainTest`：2 项失败
- `PersistenceTest`：10 项失败
- `RaftSnapshotRecoveryTest` / `RaftSnapshotRestartTest` / `RaftSnapshotDiagnosisTest`：大量失败
- `RaftIntegrationTest`：4 项失败
- `RaftSnapshotCatchupTest`：4 项失败
- `RaftSegmentStorageTest`：1 项失败
- `RaftReplicatorBehaviorTest`：2 项失败

关键失败特征：
- 多个失败直接卡在旧 `SetCommand` / `ProposeSetWithRetry(...)` 路径。
- 代表性断言：
  - `write failed, status=ApplyFailed, message=failed to parse metadata command`
  - `failed to replay committed log entries for node 1: failed to parse metadata command`
- 代表性失败文件：
  - `tests/persistence_test.cpp`
  - `tests/raft_integration_test.cpp`
  - `tests/snapshot_test.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/test_raft_replicator_behavior.cpp`

判断：
- 这批 managed failures 不是本次已迁移的 T027 / metadata 主路径回归失败。
- 主要原因是 managed tests 仍包含未迁移完成的旧 KV/`SetCommand` 断言链路；在 metadata-only 主路径下，这些路径会触发 `failed to parse metadata command`。
- 上述判断基于失败日志特征做出。

## 7. KV 残留构建依赖检查

针对 `CMakeLists.txt`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1`、`apps/`、`modules/raft/node`、`modules/raft/service`、`proto/` 做了聚焦搜索：
- 未发现 `raft_kv_client` 的生产构建引用。
- 未发现 `KvService` 或 `kv.proto` 的生产构建引用。
- `raft_demo`、`raft_metadata_client`、本轮定向测试目标均可在 Windows 构建成功。

但仍存在 KV 残留测试/脚本痕迹：
- `test.sh` 仍引用 `KvStateMachineTest`
- `test.ps1` 仍打印 `KvStateMachineTest` fallback subset
- `windows-debug-managed-tests` 仍会执行 `KvStateMachineTest.*`

结论：
- 对“已删除的 `KvService` / `raft_kv_client` / `kv.proto` 是否仍是 Windows 构建依赖”这一问题，本轮结果倾向于：否。
- 对“仓库是否已经完全无 KV 残留测试面”这一问题，本轮结果是：否，仍有 `KvStateMachineTest` 与脚本文案残留。

## 8. 验收结论

- Windows configure：通过。
- Windows 定向 build：通过。
- T027 迁移后的 leader switch / log replication / commit apply：通过。
- metadata 主路径 `MetadataStateMachineTest` / `MetadataFailoverTest` / `MetadataClientScenarioTest`：通过。
- `windows-debug-managed-tests`：已执行，但未通过；补全全量构建后仍有 39 个失败，主要集中在尚未迁移完成的旧 KV / `SetCommand` 回归路径。

综合判断：
- 以 T027 本次要求的“Windows 补测主目标”来看，结论为通过。
- 以“Windows managed tests 全面补测”来看，结论为未完全通过，仍需后续任务继续迁移旧 KV 型回归测试后再收口。
