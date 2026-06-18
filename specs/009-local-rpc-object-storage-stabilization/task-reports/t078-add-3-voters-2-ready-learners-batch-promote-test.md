# T078 Add 3 voters + 2 ready learners batch promote test

## Scope

- 任务类型：测试 / 文档 / 验证
- 本任务为 US4 batch promote 增加测试先行覆盖，锁定 `3 voters + 2 ready learners` 的安全语义。
- 本任务只修改 `tests/integrated_object_storage_quorum_test.cpp`，并补充任务报告与跨任务风险说明。
- 本任务明确不实现 `promote-to-voter`、`batch promote`、`joint consensus`、committed membership 实际变更流程。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T078
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`

## Files Changed

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t078-add-3-voters-2-ready-learners-batch-promote-test.md`
- 未修改生产代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 新增 T078 集成 quorum 测试，沿真实 `JoinMetadataCluster -> learner catch-up -> ready_to_promote / waiting_for_pair` 路径推进到第一名 learner ready。
- 在 committed membership 仍为 3 voters、quorum 仍为 2 的前提下，显式尝试接纳第二名 learner。
- 测试把期望锁定为“第二名 learner 也应可进入 learner/ready 边界，后续必须通过 batch promote 直接进入 5-voter committed membership，且不能经过 committed 4 voters”。
- 当前生产面只有单 `pending_add_learner_proposal_` 且没有 batch promote API，因此测试会在第二名 learner admission 边界暴露缺口，而不会伪造 promote 成功。

## Boundary Checks

- 没有修改生产代码
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 保持 committed membership authority 仍由 Raft 决定
- 保持 odd voter invariant
- 没有让 learner 在 promote 前进入 quorum
- 没有让 single learner 形成 committed 4-voter membership

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：`ctest --preset debug-tests -R "IntegratedObjectStorageQuorumTest\\.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory" --output-on-failure`
- 脚本命令：Not run
- 文件存在性检查：`test -f tests/integrated_object_storage_quorum_test.cpp && test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t078-add-3-voters-2-ready-learners-batch-promote-test.md` -> `PASS`
- 结果：`FAIL`
- 失败摘要：
  - 失败测试名：`IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
  - 关键断言：第二个 learner admission 期望 `METADATA_STATUS_CODE_OK` / `JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT`，实际返回 `pending AddLearner proposal already exists`
  - 失败分类：生产缺口 / 当前只支持单 `pending_add_learner_proposal_`，尚无真实 `3 voters + 2 ready learners -> batch promote -> 5 voters` 路径
  - 最后 50 行日志：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t078-ctest.log`
- 完整日志路径：
  - build：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t078-build.log`
  - test：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t078-ctest.log`

## Build Lock

- 使用 `flock` 构建锁：是
- 是否获得锁：是

## Platform Notes

- Linux：targeted build `PASS`；targeted test `FAIL`，失败点为第二个 learner 仍被 pending membership change 阻塞
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前运行时只能保留一个 pending learner，缺少 batch promote API，T078 无法在真实接口上走到“两个 ready learners -> 直接 5 voters”。
- 已同步到 `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`。
- T078 在生产 batch promote / no-committed-4-voter-history 边界实现前不应勾选完成。

## Result

- 最终状态：`PARTIAL`
- 是否可以进入下一任务：可以进入 `T079` 继续补 no-committed-4-voter-history 测试，但 `T078` 当前不能勾选完成
- 阻塞原因：缺少第二个 ready learner admission 路径和显式 batch promote / promote-to-voter API，测试无法继续验证 direct 5-voter commit 与 quorum=3
