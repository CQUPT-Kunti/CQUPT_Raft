# Windows Full Managed Failure Matrix

## 目标

把 `T034` 首次 Windows full managed CTest sweep 的当前红灯整理成单一失败矩阵。
本文件是唯一允许保留完整失败测试名的位置；其他主文档只保留摘要、状态和链接。

## 结果来源

- 复用 `T033` / `T034` 已有日志：
  - `tmp/windows-release-managed-tests.log`
  - `tmp/test-ps1-managed.log`
- `T041` focused / full managed rerun 日志：
  - `tmp/test-logs/t041-focused-runtime.log`
  - `tmp/test-logs/t041-focused-storage.log`
  - `tmp/test-logs/t041-windows-release-managed.log`

## 当前摘要

- Linux full managed CTest：`104/104` PASS
  - `platform-neutral`：100 tests PASS
  - `durability-boundary`：4 tests PASS
- Windows conservative baseline：`PASS`
  - `ctest --preset windows-release-tests`
  - 当前仍只覆盖 `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`
- Windows full managed：`FAIL`
  - `ctest --preset windows-release-managed-tests`
  - `.\test.ps1 -Managed`
  - 当前仍失败数量：`36`

## 失败分类矩阵

| 分类 | 当前状态 | 失败数量 | 典型信号 | 对应后续任务 |
|------|----------|----------|----------|--------------|
| Windows full managed CTest entry / harness 问题 | 已确认无独立阻塞 | 0 | `104` 个受管测试都已 discover 并执行；preset / wrapper 不是“没跑起来” | T036 |
| Windows runtime / timing / harness 问题 | 已收紧当前路径假设；无独立剩余项 | 0 | `raft_integration_test.cpp` 已改用更短的 Windows 临时根路径；`create temp log dir failed` 不再是当前独立 blocker | T037 |
| Windows election / replication / commit-apply 红灯 | FAIL | 4 | 剩余 cluster-style 逻辑红灯已不再被 `FlushFileBuffers ... GetLastError=5` 阻塞 | T038 |
| Windows snapshot / restart / catch-up 红灯 | FAIL | 17 | snapshot restore、restart replay、catch-up sequencing、diagnosis 断言失败；不再与目录 flush 权限错误混写 | T039 |
| Windows persistence / segment / storage 红灯 | FAIL | 6 | segment publish / recovery / clustered storage 仍有平台无关红灯 | T040 |
| Windows durability semantics adapt-or-defer | 已完成根因定位；剩余 exact seam 保持 deferred / non-equivalent | 9 | `FlushFileBuffers` 目录句柄权限问题已修复；剩余 exact failure-injection seam 不写成 Windows 已等价 Linux | T042 |
| 其他 / 待进一步分类 | 当前无独立项 | 0 | 当前 `36` 项都能先落到上面几类 | T038 / T039 / T040 / T042 |

## 受管目标状态矩阵

| 受管目标 | 状态 | 失败数量 | 主要分类 | 后续任务 |
|----------|------|----------|----------|----------|
| `test_command` | PASS | 0 | N/A | N/A |
| `test_state_machine` | PASS | 0 | N/A | N/A |
| `test_min_heap_timer` | PASS | 0 | N/A | N/A |
| `test_thread_pool` | PASS | 0 | N/A | N/A |
| `test_kv_service` | PASS | 0 | N/A | N/A |
| `test_raft_election` | FAIL | 1 | Windows election / replication / commit-apply 红灯 | T038 |
| `test_raft_log_replication` | PASS | 0 | N/A | N/A |
| `test_raft_commit_apply` | PASS | 0 | N/A | N/A |
| `test_raft_split_brain` | PASS | 0 | N/A | N/A |
| `test_t017_leader_switch_ordering` | FAIL | 2 | Windows election / replication / commit-apply 红灯 | T038 |
| `persistence_test` | FAIL | 2 | Windows durability semantics adapt-or-defer | T042 |
| `snapshot_test` | FAIL | 6 | Windows snapshot / restart / catch-up 红灯 | T039 / T042 |
| `raft_integration_test` | PASS | 0 | N/A | N/A |
| `test_raft_snapshot_catchup` | FAIL | 4 | Windows snapshot / restart / catch-up 红灯 | T039 |
| `test_raft_snapshot_restart` | FAIL | 4 | Windows snapshot / restart / catch-up 红灯 | T039 |
| `test_raft_snapshot_diagnosis` | FAIL | 4 | Windows snapshot / restart / catch-up 红灯 | T039 |
| `test_raft_segment_storage` | FAIL | 9 | Windows persistence / segment / storage 红灯 | T040 / T042 |
| `test_snapshot_storage_reliability` | FAIL | 3 | Windows durability semantics adapt-or-defer | T042 |
| `test_raft_replicator_behavior` | FAIL | 1 | Windows election / replication / commit-apply 红灯 | T038 |

## 当前失败详情

以下只保留当前仍失败的测试名；已经通过的 `CommandTest`、`KvStateMachineTest`、
`TimerSchedulerTest`、`ThreadPoolTest` 不再出现在失败矩阵中。

### 1. Windows full managed CTest entry / harness 问题

当前没有独立失败项落入这一类。现有证据更支持“入口可用，但跑出真实红灯”：

- `ctest --preset windows-release-managed-tests` 能执行完整 `104` 个受管测试
- `.\test.ps1 -Managed` 与 full managed preset 保持同一条受管测试路径
- `T041` 最新 rerun 已把 full managed 失败数从 `85` 收敛到 `36`

T036 结论：

- 当前可记为 `confirmed no entry blocker / no-op`
- 现有红灯继续转交 `T037-T041`

### 2. Windows runtime / timing / harness 问题

`T037` 当前没有独立剩余失败项。

本轮已确认的结论：

- `tests/raft_integration_test.cpp` 已在 Windows 下改用更短的临时测试根路径，
  原先的 `create temp log dir failed: 文件名或扩展名太长` 信号不再出现。
- `T041` 修复 Windows 目录 sync 句柄权限后，
  `RaftKvServiceTest.SingleNodeSupportsPutGetDeleteAndStatusHealth`、
  `RaftKvServiceTest.ThreeNodeFollowerRedirectsWritesAndReadsToLeader`、
  `RaftIntegrationTest.ElectsSingleLeaderInThreeNodeCluster`、
  `RaftIntegrationTest.ReplicatesSetAndDeleteCommandsToAllNodes`、
  `RaftIntegrationTest.ElectsNewLeaderAfterCurrentLeaderStops`、
  `RaftIntegrationTest.GeneratesSnapshotMetaFileAfterEnoughAppliedLogs`、
  `RaftIntegrationTest.LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary`
  已从失败矩阵移除。

因此，原先暂放在 `T037` 的 7 个失败已完成收口；当前没有独立的
Windows runtime / timing / harness blocker。

### 3. Windows election / replication / commit-apply 红灯

这些失败优先进入 `T038`，处理平台无关 Raft election / replication /
commit-apply 逻辑在 Windows full managed sweep 下的剩余红灯：

- `RaftElectionTest.FollowerRejectsClientProposeAfterLeaderIsElected`
- `RaftLeaderSwitchOrderingTest.CommittedStateSurvivesLeaderSwitchAndNewLeaderContinuesReplication`
- `RaftLeaderSwitchOrderingTest.LaggingFollowerCatchesUpDuringLeaderSwitchWithoutCommitApplyReordering`
- `RaftReplicatorBehaviorTest.SlowFollowerCatchesUpWhileLeaderKeepsAcceptingNewLogs`

### 4. Windows snapshot / restart / catch-up 红灯

这些失败优先进入 `T039`，处理 snapshot、restart、catch-up、diagnosis 相关
红灯：

- `RaftSnapshotRecoveryTest.FullRestartReplaysSnapshotTailWithoutLosingDeletesOrOverwrites`
- `RaftSnapshotRecoveryTest.RestartedFollowerAppliesCommittedTailExactlyOnceAfterSnapshotLoad`
- `RaftSnapshotRecoveryTest.StandaloneRestartFallsBackToOlderTrustedSnapshotWhenNewestSnapshotIsCorrupted`
- `RaftSnapshotRecoveryTest.RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`
- `RaftSnapshotRecoveryTest.StandaloneRestartRejectsMetadataMismatchedVisibleSnapshotAndKeepsTrustedBoundary`
- `RaftSnapshotRecoveryTest.AllPublishedSnapshotsInvalidYieldNoTrustedSnapshot`
- `RaftSnapshotCatchupTest.RestartedFollowerCatchesUpLargeGapWithBatchedAppendEntries`
- `RaftSnapshotCatchupTest.LaggingFollowerReplaysLiveLogWithoutBreakingCommittedDeleteOrdering`
- `RaftSnapshotCatchupTest.RestartedFollowerInstallsSnapshotWhenLeaderCompactedLogs`
- `RaftSnapshotCatchupTest.FollowerContinuesReplicatingLogsAfterInstallingSnapshot`
- `RaftSnapshotRestartTest.FollowerKeepsStateAfterInstallSnapshotAndRestart`
- `RaftSnapshotRestartTest.LeaderKeepsCompactedSnapshotStateAfterRestart`
- `RaftSnapshotRestartTest.FullClusterRestartsAfterSnapshotAndContinuesWriting`
- `RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart`
- `RaftSnapshotDiagnosisTest.RestartedSingleNodeLoadsSnapshotAndTailLogsWithoutPeers`
- `RaftSnapshotDiagnosisTest.CompactedClusterReplicatesNewLogAfterRestartedLeaderStepsDown`
- `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- `RaftSnapshotDiagnosisTest.RestartedSingleNodeSkipsMetadataMismatchedVisibleSnapshotAndReplaysCommittedTail`

### 5. Windows persistence / segment / storage 红灯

这些失败优先进入 `T040`，处理 platform-neutral 的 persistence / segment /
storage 恢复与路径问题：

- `RaftSegmentStorageTest.MissingFirstSegmentFailsBeforeTrustingPublishedBoundary`
- `RaftSegmentStorageTest.FinalSegmentTailTruncateKeepsTrustedLogPrefixAndClampsCommitApply`
- `RaftSegmentStorageTest.UnsupportedMetaVersionFailsLoadWithPathAndVersion`
- `RaftSegmentStorageTest.InconsistentMetaLogBoundaryFailsBeforeTrustingSegments`
- `RaftSegmentStorageTest.CorruptedEarlierSegmentTailCleansLaterSegmentsAndReportsDiagnostics`
- `RaftSegmentStorageTest.RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory`

### 6. Windows durability semantics adapt-or-defer

`T041` 已确认 `FlushFileBuffers ... GetLastError=5` 的根因是 Windows 目录句柄
只用 `FILE_LIST_DIRECTORY` 打开，导致 `FlushFileBuffers` 在普通目录上触发
`ERROR_ACCESS_DENIED`。当前 storage / snapshot storage 都已改为用可写目录句柄
执行 directory sync，因此下列 7 个此前被该问题阻塞的 cluster-style 用例已经转绿，
并从失败矩阵中删除：

- `RaftKvServiceTest.SingleNodeSupportsPutGetDeleteAndStatusHealth`
- `RaftKvServiceTest.ThreeNodeFollowerRedirectsWritesAndReadsToLeader`
- `RaftIntegrationTest.ElectsSingleLeaderInThreeNodeCluster`
- `RaftIntegrationTest.ReplicatesSetAndDeleteCommandsToAllNodes`
- `RaftIntegrationTest.ElectsNewLeaderAfterCurrentLeaderStops`
- `RaftIntegrationTest.GeneratesSnapshotMetaFileAfterEnoughAppliedLogs`
- `RaftIntegrationTest.LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary`

剩余 exact seam 用例不再以 `FlushFileBuffers GetLastError=5` 这种目录 flush 权限错误
失败，但它们依然不能被写成“Windows 已等价验证 Linux-specific durability /
failure-injection”。这些项在本轮按 `deferred / non-equivalent` 收口，留给 `T042`
做最终文档化关闭：

- `PersistenceTest.MetaFileSyncFailureNeedsExactFailureInjectionSeam`
- `PersistenceTest.MetaDirectorySyncFailureNeedsExactFailureInjectionSeam`
- `RaftSnapshotRecoveryTest.RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`
- `RaftSegmentStorageTest.LogDirectoryReplaceFailureNeedsExactFailureInjectionSeam`
- `RaftSegmentStorageTest.LogDirectorySyncFailureNeedsExactFailureInjectionSeam`
- `RaftSegmentStorageTest.FinalSegmentPartialWriteNeedsExactFailureInjectionSeam`
- `SnapshotStorageReliabilityTest.StagedSnapshotPublishFailureNeedsExactFailureInjectionSeam`
- `SnapshotStorageReliabilityTest.SnapshotDirectorySyncFailureNeedsExactFailureInjectionSeam`
- `SnapshotStorageReliabilityTest.SnapshotPruneRemoveFailureNeedsExactFailureInjectionSeam`

### 7. 其他 / 待进一步分类

当前没有独立失败项落入这一类。若后续 `T036` / `T037` 发现某些失败实际上属于
preset、discover、working directory、multi-config、output directory 或 test
filter 问题，再从本矩阵迁出并重分配。
