# T086 Record Batch Promote Validation

## Scope

- 任务类型：验证记录 / 风险同步 / 任务状态收口
- 本任务是 US4 batch promote / odd-voter membership safety 的收口验证任务。
- 本任务不写生产代码，不改测试逻辑，只记录 T078-T085 完成后的当前语义、Linux 定向验证结果、平台 pending 状态与残余风险。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t086-record-batch-promote-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Phase Summary

- T078: report found。最初单独执行结果为 `PARTIAL`，当时第二个 learner 仍受单 pending learner 边界阻塞；当前仓库状态已由 T082-T085 补齐真实 batch promote 路径，`tasks.md` 中已标记完成。
- T079: report found。`PASS`。已对 no committed `4-voter` history 增加断言。
- T080: report found。`PASS`。已覆盖 leader failure during batch promote 不留下 partial committed membership。
- T081: report found。`PASS`。已覆盖 committed `5-voter` batch membership 的 restart recovery。
- T082: report found。`PASS`。已在 `RaftNode` 内形成原子 batch learner promotion safety boundary。
- T083: report found。`PASS`。已通过 `JoinMetadataCluster` service 边界把 promote 路由到 `RaftNode` 原子提交路径。
- T084: report found。`PASS`。已把 target voter odd-count 校验前移到 membership proposal commit 前。
- T085: report found。`PASS`。已把两个 ready learners 一起 promote，并在成功响应中反映 `5 voters / quorum 3`。

## Current Batch Promote Semantics

- single learner promote 被阻止；`3 voters + 1 learner` 不会形成 committed `4-voter membership`。
- learner ready-to-promote 不等于自动成为 voter；ready 状态本身不改变 committed membership。
- 只有 `3 voters + 2 ready learners` 才能进入当前实现支持的 batch promote 目标。
- batch promote 目标是 committed `5 voters`，promote 成功后 quorum 为 `3`。
- quorum、election、commit 仍只按 committed voters 计算，learners 在 promote 前不参与 quorum / election。
- committed membership history 不应出现 committed `4 voters` 中间态。
- leader failure during batch promote 不应留下 partial committed membership，也不应把 interrupted promote 恢复成 committed voters。
- snapshot / restart recovery 后，committed batch membership 应恢复为 committed `5 voters / quorum 3`，不会恢复出 committed `4-voter` state。
- ViewNode observation 仍然只是 discovery / observation，不是 promote authority。
- committed membership 仍必须由 Raft log / committed config path 决定，不由 observed state、ready learner 状态或 ViewNode 信息直接决定。

## Linux Validation

- build 命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum test_metadata_failover test_raft_snapshot_restart ) 9>/tmp/cqupt_raft_build.lock`
- test 命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataFailover|RaftSnapshotRestart|RaftSnapshotRecovery|SnapshotRestart" --output-on-failure`
- 结果：`PASS`
- 日志路径：
  - build：`tmp/test-logs/t086-build.log`
  - test：`tmp/test-logs/t086-ctest.log`

Linux 本次实际覆盖结论：

- `integrated_object_storage_quorum`: `PASS`
  - `IntegratedObjectStorageQuorumTest.ThreeVotersPlusObservedLearnerKeepsCommittedQuorumAtTwo`
  - `IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`
  - `IntegratedObjectStorageQuorumTest.SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal`
  - `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
  - `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
- `metadata_failover_test`: `PASS`
  - `MetadataFailoverTest.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership`
- `test_raft_snapshot_restart`: `PASS`
  - `RaftSnapshotRestartTest.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership`
  - 同次回归匹配到的 `RaftSnapshotRecoveryTest.*` 也全部通过

本次定向 CTest 汇总：

- `36/36` PASS
- `Total Test time (real) = 253.34 sec`

## Windows Validation

- Windows: pending / not run

## macOS Validation

- macOS: pending / not run

## Skipped Checks

- local RPC dynamic metadata join + batch promote smoke：not run。Phase 10 example 级工作尚未完成。
- long-running failover / soak：not run。当前只有 targeted CTest，缺少长时间故障切换 soak。
- multi-ViewNode discovery 对 promote target 选择影响的运行时 smoke：not run。当前只保留“ViewNode 非 authority”边界与 targeted 测试结论。
- Windows：not run，无实机 / 对应运行环境。
- macOS：not run，无实机 / 对应运行环境。

## Remaining Risks / Follow-ups

- joint consensus 仍未实现，也未做完整验证；当前安全边界来自原子 batch membership transition，而不是完整 joint consensus 协议。
- batch promote failover 当前只有 targeted CTest 覆盖，缺少 long-running failover / soak 证据。
- snapshot / restart recovery 已覆盖当前关键 committed `5-voter` 语义，但 dedicated membership history / traceability 仍不是 first-class 通道。
- local RPC dynamic metadata join + batch promote smoke 仍缺失，当前还不能把 US4 结论扩张成完整运行时 example workflow PASS。
- multi-ViewNode discovery / observation 对 promote target 暴露与操作路径的影响仍未做完整运行时验证；当前只能确认 ViewNode observation 不会直接触发 promote。
- Windows / macOS 仍未实测，必须继续保持 pending / not run，不能写 PASS。
- 当前未观察到 partial promote、duplicate membership、stale promote 导致 committed membership 污染的证据；但该结论仍以现有 targeted tests 为边界，不代表已完成长期并发/重复请求 soak。

## Result

- 最终状态：`PASS`
- `tasks.md`：已只勾选 `T086`
- 是否可以进入 `T087`：可以进入后续任务判断；但按当前结果，safe batch promote 已完成，`T087` 原始“cannot be finished / blocked promote”前提已不成立。
