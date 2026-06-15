# T080 Add leader failure during batch promote test

## Scope

- 任务类型：测试 / 文档 / 验证
- 本任务为 US4 batch promote failover safety 增加 leader failure 场景测试先行覆盖。
- 本任务只修改 `tests/metadata_failover_test.cpp`，并补充任务报告与跨任务风险说明。
- 本任务明确不实现 `promote-to-voter`、`batch promote`、`joint consensus`、新的 failover 生产逻辑或 committed membership 实际变更流程。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T080
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t078-add-3-voters-2-ready-learners-batch-promote-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t079-add-no-committed-4-voter-history-assertion.md`

## Files Changed

- `tests/metadata_failover_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t080-add-leader-failure-during-batch-promote-test.md`
- 未修改生产代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 为 `metadata_failover_test.cpp` 增加 JoinMetadataCluster / detached learner / runtime membership / committed membership diagnostics 辅助断言。
- 新增 T080 failover 测试，沿真实 `learner ready_to_promote / waiting_for_pair -> second learner join attempt -> leader failover -> new leader retry` 路径验证 failover safety。
- 测试锁定当前可观察安全语义：leader failure 之后 committed membership 仍为 3 voters、quorum 仍为 2、没有 committed 4-voter state、learners 没有被错误恢复为 voters、retry 不会扩成重复 membership entry。
- 当前生产面仍缺少 two-ready-learners / batch promote API，所以测试会在进一步要求 promote-in-progress 边界时暴露缺口，而不会伪造 batch promote 成功。

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

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_metadata_failover ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：`ctest --preset debug-tests -R "MetadataFailoverTest\\.(NewLeaderKeepsCommittedVisibleAndPendingHidden|SameCommitRequestIdCanBeRetriedOnNewLeader|LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership)" --output-on-failure`
- 脚本命令：Not run
- 文件存在性检查：`test -f tests/metadata_failover_test.cpp && test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t080-add-leader-failure-during-batch-promote-test.md` -> `PASS`
- 结果：`FAIL`
- 失败摘要：
  - 通过测试：`MetadataFailoverTest.NewLeaderKeepsCommittedVisibleAndPendingHidden`、`MetadataFailoverTest.SameCommitRequestIdCanBeRetriedOnNewLeader`
  - 失败测试名：`MetadataFailoverTest.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership`
  - 关键断言：第二个 learner admission 期望 `METADATA_STATUS_CODE_OK` / `JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT`，实际返回 `pending AddLearner proposal already exists`
  - 失败分类：生产缺口 / 当前只支持单 `pending_add_learner_proposal_`，无法形成真实 `2 ready learners + promote in progress` failover 边界
  - 最后 50 行日志：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t080-ctest.log`
- 完整日志路径：
  - build：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t080-build.log`
  - test：`/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t080-ctest.log`

## Build Lock

- 使用 `flock` 构建锁：是
- 是否获得锁：是

## Platform Notes

- Linux：targeted build `PASS`；两个现有 failover 回归 `PASS`；T080 新测试 `FAIL`
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前 failover harness 能验证的是 blocked / partial / interrupted promote path 的安全语义，还不能表达真实 two-ready-learners batch promote in progress。
- 当前运行时缺少 first-class committed membership history trace，failover 后只能通过 committed quorum summary、runtime summary 和 JoinMetadataCluster diagnostics 观察状态。
- 已同步到 `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`。

## Result

- 最终状态：`PARTIAL`
- 是否可以进入下一任务：可以进入 `T081` 继续补 restart recovery 测试，但 `T080` 当前不能勾选完成
- 阻塞原因：当前运行时无法形成真实 batch-promote-in-progress boundary，测试只能锁定 blocked / partial / interrupted promote path 的 failover safety，无法完成 `2 ready learners + leader failure during batch promote` 全路径验证
