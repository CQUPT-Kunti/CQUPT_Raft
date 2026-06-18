# T088 Record Batch Promote Validation

## Scope

- 任务类型：验证记录 / 文档收口
- 本任务只记录 Phase 9 batch promote 的最终验证状态。
- 本任务不写生产代码，不修改测试断言，不弱化现有测试结论。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t088-record-batch-promote-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## T078-T087 Status Summary

- T078：历史结果 `PARTIAL`
  - 当时 `3 voters + 2 ready learners` 仍被单 pending learner 边界阻塞，尚不能证明 direct `3 -> 5` committed voters。
  - 当前该历史阻塞已被后续任务补齐，不再代表仓库现状。
- T079：`PASS`
  - 已对 no committed `4-voter` history 增加断言。
- T080：`PASS`
  - 已覆盖 leader failure during batch promote 不留下 partial committed membership。
- T081：`PASS`
  - 已覆盖 committed batch membership 的 restart recovery。
- T082：`PASS`
  - 已在 `RaftNode` 内建立原子 batch learner promotion safety boundary。
- T083：`PASS`
  - 已通过 `JoinMetadataCluster` service boundary 把 batch promote 路由到 Raft log / committed config path。
- T084：`PASS`
  - 已在 membership proposal commit 前阻止 even-voter target。
- T085：`PASS`
  - 已支持两个 ready learners 一起 promote，并反映 `5 voters / quorum 3`。
- T086：`PASS`
  - 已记录 Linux targeted batch promote 验证、failover / restart / no-`4-voter` safety 与 residual risks。
- T087：`PASS`
  - 已确认当前里程碑不需要降级写成 learner-only / blocked promote fallback。

## Current Phase 9 Judgment

- 最终判断：`PASS`
- Phase 9 batch promote 当前可按 targeted Linux 验证闭环写成 complete。
- 前提说明：
  - 该 `PASS` 只覆盖当前 targeted build/CTest safety boundary。
  - 不代表 Phase 10 local RPC example workflow 已完成。
  - 不代表 Windows/macOS 已实测通过。

## Batch Promote Completion

- `3 voters + 2 ready learners` 当前可以安全 batch promote 到 committed `5 voters`。
- committed voter set 当前证据支持 direct `3 -> 5`，不能观察到 committed `4-voter` 中间态。
- promote 前 quorum 保持 committed-voters-only 语义；`3 voters + 1 learner` 仍是 quorum `2`。
- promote 成功后 committed quorum 可提升到 `3`。

## Linux Validation

- build 命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum test_metadata_failover test_raft_snapshot_restart ) 9>/tmp/cqupt_raft_build.lock`
- test 命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataFailover|Failover|RaftSnapshotRestart|SnapshotRestart" --output-on-failure`
- 结果：`PASS`
- 通过摘要：
  - `30/30` PASS
  - 总耗时：`141.06 sec`
- 日志路径：
  - `tmp/test-logs/t088-build.log`
  - `tmp/test-logs/t088-ctest.log`
- 说明：
  - 本次 regex 中的 `Failover` 额外匹配到 `ViewNodeDiscoveryTest.IntegrationFailoverDiscoveryUsesSurvivorObservedRegistryState`，该项也通过，但不作为 Phase 9 batch promote 核心证据。

## Key Validation Evidence

- committed voters 是否能从 `3` 直接到 `5`：`PASS`
  - `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
- quorum 是否从 `2` 到 `3`：`PASS`
  - promote 前：
    - `IntegratedObjectStorageQuorumTest.ThreeVotersPlusObservedLearnerKeepsCommittedQuorumAtTwo`
  - promote 后：
    - `IntegratedObjectStorageQuorumTest.FiveVoterCommittedMembershipKeepsQuorumThreeAndAllowsCommitWithThreeReachableVoters`
- 是否证明没有 committed `4-voter` history：`PASS`
  - `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
  - `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
- leader failure during batch promote：`PASS`
  - `MetadataFailoverTest.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership`
- committed batch membership restart recovery：`PASS`
  - `RaftSnapshotRestartTest.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership`
- duplicate / concurrent pending membership change：`PASS`
  - `IntegratedObjectStorageQuorumTest.AddLearnerProposalPathRejectsFollowerAndPreservesDuplicatePendingBoundary`
  - `IntegratedObjectStorageQuorumTest.JoinMetadataClusterLeaderValidatesInvalidDuplicateAndPendingWithoutChangingCommittedMembership`
  - 结合 T086 既有结论，当前未观察到 duplicate / pending change 导致 committed membership 污染。
- learner-only / blocked promote fallback 是否仍需要记录：`NO`
  - T087 已确认 safe batch promote 已完成 targeted Linux 验证，不能再把当前仓库写成 learner-only completion。

## Skipped / Pending

- Windows：`pending / not run`
- macOS：`pending / not run`
- local RPC dynamic metadata join + batch promote smoke：`pending / not run`
- long-running failover / duplicate-request soak：`pending / not run`
- multi-ViewNode runtime observation/promote interaction smoke：`pending / not run`
- joint consensus implementation / protocol-level validation：`pending / not run`

## Snapshot Cleanup And Rerun-Failed

- snapshot cleanup：`not run`
- `ctest --rerun-failed`：`not run`
- 原因：本次 targeted CTest 首轮即 `30/30 PASS`，没有失败项，也没有触发 snapshot 清理前提。

## Cross-Task Risk Review

- 本次未发现需要新增到 `cross-task-risk-notes.md` 的新跨任务风险。
- 当前残余风险仍与 T086/T087 一致：
  - 结论范围仍限于 targeted Linux safety boundary；
  - local RPC example workflow 仍未完成；
  - Windows/macOS 仍未实测。

## Result

- 最终状态：`PASS`
- 是否已勾选 `T088`：是
- 是否可以进入 `T089`：可以
