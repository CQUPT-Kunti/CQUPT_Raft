# T043 US4 recovery/snapshot/catch-up Linux 验证

## 范围
- 本任务仅执行 Linux 验证与报告。
- 本轮覆盖 T036-T042 的以下范围：
  - metadata snapshot V2
  - MetadataStateMachine save/load/replay
  - RaftNode startup load + post-snapshot replay
  - SnapshotStorage metadata-only 诊断
  - restart recovery
  - follower catch-up
  - split brain / snapshot diagnosis
  - metadata recovery stress
  - 已迁移的 metadata-only Raft 回归测试

## Linux 命令
- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_snapshot test_metadata_state_machine test_metadata_recovery_stress snapshot_test persistence_test test_raft_snapshot_catchup test_raft_snapshot_restart test_raft_snapshot_diagnosis test_raft_split_brain test_raft_replicator_behavior test_raft_segment_storage test_snapshot_storage_reliability`
- `ctest --test-dir build/linux --output-on-failure -R "(MetadataSnapshotTest|MetadataStateMachineTest|MetadataRecoveryStressTest|RaftSnapshotRecoveryTest|PersistenceTest|RaftSnapshotCatchupTest|RaftSnapshotRestartTest|RaftSnapshotDiagnosis|RaftSplitBrain|RaftReplicatorBehavior|RaftSegmentStorage|SnapshotStorageReliability)"`
- 补充说明：
  - `PersistenceMoreTest` 不存在于当前 CTest 名称中。
  - `tests/persistence_more_test.cpp` 目前仍是 manual-only 程序，不在 `tests/CMakeLists.txt` target 内。
  - 对该文件额外执行了 Linux 语法检查：`/usr/bin/c++ ... -fsyntax-only tests/persistence_more_test.cpp`

## Linux configure / build 结果
- Configure：PASS
- Build：PASS
- `persistence_more_test.cpp` syntax-only：PASS

## Linux CTest 结果
- 总体：FAIL
- 汇总：116 个测试中 95 个 PASS，21 个 FAIL
- 总耗时：`474.10 sec`

## 已通过覆盖
- `MetadataStateMachineTest`：PASS
  - 覆盖 metadata state machine V2、snapshot save/load、replay、idempotency、boundary 校验。
- `MetadataSnapshotTest`：PASS
  - 覆盖 metadata snapshot V2 header、version、corruption/magic/version 错误。
- `MetadataRecoveryStressTest`：PASS
  - 覆盖并发 apply/query、restart recovery、follower catch-up 的 metadata facts。
- `RaftSplitBrainTest`：PASS
  - 覆盖 split brain 后 metadata-only 提案/拒绝/InstallSnapshot 行为。
- `RaftSnapshotDiagnosisTest`：PASS
  - 覆盖 corrupted snapshot、metadata mismatch、startup load、tail replay 诊断。
- `RaftSegmentStorageTest`：PASS
  - 覆盖 meta/log trusted boundary、partial/corrupt publish、segment recovery。
- `SnapshotStorageReliabilityTest`：PASS
  - 覆盖 staging publish、checksum、metadata snapshot boundary mismatch、version/header 错误、目录同步失败注入。
- `RaftReplicatorBehaviorTest`：PASS
  - 覆盖慢 follower 不阻塞多数派提交、catch-up 期间继续接受新日志。

## 失败项
- `PersistenceTest` 失败 9 项：
  - `RestartedFollowerCatchesUp`
  - `ColdRestartClampsCommitAndApplyBoundariesToLastLogIndex`
  - `ColdRestartUsesPreviouslyTrustedMetaBoundaryWhenNewLogPublishesBeforeMeta`
  - `ColdRestartClampsCommitIndexToLastLogAndReplaysCommittedPrefix`
  - `ColdRestartClampsLastAppliedToCommitIndexWhenAppliedExceedsCommit`
  - `ColdRestartClampsLastAppliedToTrustedLogPrefixWhenAppliedPointsPastAvailableLog`
  - `ColdRestartUsesOlderMetaTermAndVoteWhenNewerLogTreeIsVisible`
  - `MetaFileSyncFailureNeedsExactFailureInjectionSeam`
  - `MetaDirectorySyncFailureNeedsExactFailureInjectionSeam`
- `RaftSnapshotCatchupTest` 失败 3 项：
  - `RestartedFollowerCatchesUpLargeGapWithBatchedAppendEntries`
  - `LaggingFollowerReplaysLiveLogWithoutBreakingCommittedDeleteOrdering`
  - `RestartedFollowerInstallsSnapshotWhenLeaderCompactedLogs`
- `RaftSnapshotRestartTest` 失败 3 项：
  - `FollowerKeepsStateAfterInstallSnapshotAndRestart`
  - `LeaderKeepsCompactedSnapshotStateAfterRestart`
  - `FullClusterRestartsAfterSnapshotAndContinuesWriting`
- `RaftSnapshotRecoveryTest` 失败 6 项：
  - `FullRestartReplaysSnapshotTailWithoutLosingDeletesOrOverwrites`
  - `RestartedFollowerAppliesCommittedTailExactlyOnceAfterSnapshotLoad`
  - `StandaloneRestartFallsBackToOlderTrustedSnapshotWhenNewestSnapshotIsCorrupted`
  - `RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`
  - `StandaloneRestartRejectsMetadataMismatchedVisibleSnapshotAndKeepsTrustedBoundary`
  - `AllPublishedSnapshotsInvalidYieldNoTrustedSnapshot`

## 失败共性与原因
- 失败日志显示多个恢复/重启/catch-up case 仍经由 `tests/support/raft_snapshot_restart_test_utils.h` 提交 `SetCommand(...)`。
- metadata-only 主路径下这些命令被状态机拒绝，统一表现为：
  - `state machine apply failed`
  - `failed to parse metadata command`
- 直接后果：
  - 写入阶段停在 very early apply failure
  - `last_snapshot_index` 无法推进
  - 后续 restart recovery / catch-up / replay 断言连带失败
- 这说明：
  - T036/T038/T041/T042 相关 metadata-only 验证面已具备并通过
  - 但 `PersistenceTest` / `RaftSnapshotCatchupTest` / `RaftSnapshotRestartTest` / `RaftSnapshotRecoveryTest` 仍残留 KV helper 路径，US4 Linux 总体验证尚未完全转绿

## KV removal status
- 已完成：
  - `test_raft_split_brain.cpp`
  - `test_raft_snapshot_diagnosis.cpp`
  - `persistence_more_test.cpp`（manual-only）
  - `test_raft_replicator_behavior.cpp`
  - `test_raft_segment_storage.cpp`
- 未完成残留：
  - 恢复/重启 helper `tests/support/raft_snapshot_restart_test_utils.h` 仍在使用 KV `SetCommand(...)`
  - 这也是本轮 21 个失败项的主阻塞

## durability / replay 影响
- durability 相关正向信号：
  - `SnapshotStorageReliabilityTest` 全 PASS
  - `RaftSegmentStorageTest` 全 PASS
  - `MetadataSnapshotTest` 全 PASS
- replay / restart / catch-up 当前结论：
  - metadata-only 路径在已迁移测试上可工作
  - 但尚不能宣称 US4 Linux recovery/catch-up 全量通过，因为仍有旧 KV helper 阻塞多个 recovery suite

## Windows 说明
- 当前仅完成 Linux 验证，Windows 留待后续 Windows 环境补测。
- 本报告不写 Windows PASS。

## 未执行项
- 未执行 `PersistenceMoreTest` CTest：
  - 原因：当前仓库没有该 CTest 名称或对应 target。
  - 已改用 `tests/persistence_more_test.cpp` 的 Linux syntax-only 作为最小补充验证。
