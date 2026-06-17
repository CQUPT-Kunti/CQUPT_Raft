# T087 Document Learner-Only Completion And Blocked Promote Status

## Scope

- 任务类型：文档判断 / 验证复核 / 风险结论
- 本任务只复核 T084/T085/T086 当前状态，并判断是否需要把 US4 结论降回 learner-only / blocked promote 风险。
- 本任务不实现生产代码，不修改测试断言，不改 `cross-task-risk-notes.md` 中已成立的 PASS 结论。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t087-document-learner-only-completion-and-blocked-promote-status.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Current T084/T085/T086 Status

- T084：`PASS`
  - 已在 committed membership proposal commit 前完成 target voter odd-count 校验。
  - `3 voters + 1 ready learner` 会在进入 membership proposal 前被拒绝，不会形成 committed `4-voter` 中间态。
- T085：`PASS`
  - 已通过 `JoinMetadataCluster` service 路由把 `2` 个 ready learners 一起 promote 到 committed `5 voters`。
  - 成功响应已反映 `committed_voter_count=5` 与 `committed_quorum_size=3`。
- T086：`PASS`
  - 已记录 Linux targeted batch promote 验证、残余风险和平台 pending 状态。
  - `cross-task-risk-notes.md` 当前已经把 US4 safe batch promote 写成 targeted Linux safety boundary 完成，而不是 learner-only fallback。

## Current Re-Validation

- 构建命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure`
- 结果：`PASS`
- 通过摘要：
  - `IntegratedObjectStorageQuorum`: `14/14` PASS
  - 总耗时：`37.35 sec`
- 关键通过用例：
  - `IntegratedObjectStorageQuorumTest.ThreeVotersPlusObservedLearnerKeepsCommittedQuorumAtTwo`
  - `IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`
  - `IntegratedObjectStorageQuorumTest.SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal`
  - `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
  - `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
- 日志路径：
  - `tmp/test-logs/t087-build.log`
  - `tmp/test-logs/t087-ctest.log`

## Judgment

- 最终判断：`PASS`
- safe batch promote 在当前里程碑内已经完成 targeted Linux 验证，不能降级写成“当前只能 learner join / catch-up / blocked single promote”。
- 当前证据已经支持以下结论：
  - single learner promote 继续被阻止；
  - `3 voters + 2 ready learners -> committed 5 voters / quorum 3` 已有实测通过；
  - committed membership history 不暴露 committed `4-voter` 中间态。
- 因此：
  - 不新增 learner-only fallback 风险；
  - 不修改 `cross-task-risk-notes.md`；
  - 不修改 `validation-matrix.md`。

## Residual Boundary

- Phase 10 local RPC example 仍未完成，当前仍不能把 example workflow 写成已完成的动态 metadata join + batch promote smoke。
- 该未完成项属于 example/runtime workflow pending，不属于“safe batch promote 未完成”或“只能 learner-only + blocked promote”的回退状态。
- Windows/macOS 仍是 pending / not run。

## Result

- `tasks.md`：本任务可勾选 `T087`
- 是否需要新增 blocked promote 风险记录：否
- 是否可以进入下一任务：可以进入 `T088`
