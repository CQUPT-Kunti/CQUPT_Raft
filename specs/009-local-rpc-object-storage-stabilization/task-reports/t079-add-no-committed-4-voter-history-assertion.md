# T079 Add no committed 4-voter history assertion

## Scope

- 任务类型：测试 / 文档 / 验证
- 本任务为 US4 batch promote safety 增加 no committed 4-voter history 测试先行覆盖。
- 本任务只修改 `tests/integrated_object_storage_quorum_test.cpp`，并补充任务报告与跨任务风险说明。
- 本任务明确不实现 `promote-to-voter`、`batch promote`、`joint consensus`、committed membership 实际变更流程。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T079
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t078-add-3-voters-2-ready-learners-batch-promote-test.md`

## Files Changed

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t079-add-no-committed-4-voter-history-assertion.md`
- 未修改生产代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 增加 committed membership diagnostics 辅助解析与 no-4-voter-history 断言。
- 新增 T079 集成 quorum 测试，沿真实 `JoinMetadataCluster -> learner catch-up -> ready_to_promote / waiting_for_pair -> second learner join attempt -> leader failover` 路径采集可观察 committed state。
- 测试同时检查 committed quorum summary 与 JoinMetadataCluster 诊断消息，确保 blocked / partial / interrupted 路径里都看不到 committed 4-voter membership。
- 当前生产面仍缺少第二 ready learner admission 与 batch promote history trace，所以测试会在进一步要求 `3 voters + 2 ready learners` 时暴露缺口，而不会伪造历史不存在。

## Boundary Checks

- 没有修改生产代码
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 保持 committed membership authority 仍由 Raft 决定
- 保持 odd voter invariant
- 没有让 learner 在 promote 前进入 quorum
- 没有让 single learner promote 成功
- 没有伪造 4-voter committed membership

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：`ctest --preset debug-tests -R "IntegratedObjectStorageQuorumTest\\.(SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount|JoinMetadataClusterReportsPendingThenReadyLearnerWaitingForPair|BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory)" --output-on-failure`
- 脚本命令：Not run
- 文件存在性检查：`test -f tests/integrated_object_storage_quorum_test.cpp && test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t079-add-no-committed-4-voter-history-assertion.md` -> `PASS`
- 结果：`FAIL`
- 失败摘要：
  - 通过测试：`IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`、`IntegratedObjectStorageQuorumTest.JoinMetadataClusterReportsPendingThenReadyLearnerWaitingForPair`
  - 失败测试名：`IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
  - 关键断言：第二个 learner admission 期望 `METADATA_STATUS_CODE_OK` / `JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT`，实际返回 `pending AddLearner proposal already exists`
  - 失败分类：生产缺口 / 当前只支持单 `pending_add_learner_proposal_`，尚无真实 `3 voters + 2 ready learners -> batch promote -> 5 voters` 路径，也没有独立 committed membership history trace
  - 最后 50 行日志：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t079-ctest.log`
- 完整日志路径：
  - build：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t079-build.log`
  - test：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t079-ctest.log`

## Build Lock

- 使用 `flock` 构建锁：是
- 是否获得锁：是

## Platform Notes

- Linux：targeted build `PASS`；T070/T076 回归 `PASS`；T079 新测试 `FAIL`
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前可观察边界主要是 committed quorum summary 与 JoinMetadataCluster diagnostics，还没有独立 membership history trace / committed config timeline。
- 当前运行时只能保留一个 pending learner，缺少 batch promote API，T079 无法在真实接口上走到“两个 ready learners -> 直接 5 voters -> quorum 3”。
- 已同步到 `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`。

## Result

- 最终状态：`PARTIAL`
- 是否可以进入下一任务：可以进入 `T080` 继续补 batch promote failover 测试，但 `T079` 当前不能勾选完成
- 阻塞原因：缺少第二个 ready learner admission 路径与 first-class committed membership history trace，测试只能证明当前 blocked / partial / interrupted 可观察状态中没有 committed 4 voters，无法完成 `3 -> 5` 全路径验证
