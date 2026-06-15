# T081 Add restart recovery for committed batch membership

## Scope

- 任务类型：测试 / 文档 / 验证
- 本任务为 US4 committed batch membership restart recovery 增加测试先行覆盖。
- 本任务只修改 `tests/test_raft_snapshot_restart.cpp`，并补充任务报告与跨任务风险说明。
- 本任务明确不实现 `promote-to-voter`、`batch promote`、`joint consensus`、新的 restart recovery 生产逻辑或 committed membership 实际变更流程。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T081
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t078-add-3-voters-2-ready-learners-batch-promote-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t079-add-no-committed-4-voter-history-assertion.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t080-add-leader-failure-during-batch-promote-test.md`

## Files Changed

- `tests/test_raft_snapshot_restart.cpp`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t081-add-restart-recovery-for-committed-batch-membership.md`
- 未修改生产代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 为 `test_raft_snapshot_restart.cpp` 增加 JoinMetadataCluster / detached learner / committed membership / runtime membership 诊断辅助断言。
- 新增 T081 restart 测试，沿真实 `snapshot baseline -> learner ready_to_promote / waiting_for_pair -> second learner join attempt -> full restart -> retry join` 路径验证 restart safety。
- 测试锁定当前可观察恢复语义：restart 后 committed membership 仍为 3 voters、quorum 仍为 2、没有 committed 4-voter state、learners 没有被恢复成 voters、blocked/partial promote 不会被恢复成 committed membership。
- 当前生产面仍缺少真实 `3 voters + 2 ready learners -> committed 5 voters` 批量 membership 变更及其持久化恢复，因此测试会在要求恢复出 committed 5-voter membership 时暴露缺口，而不会伪造恢复成功。

## Boundary Checks

- 没有修改生产代码
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 保持 committed membership authority 仍由 Raft 决定
- 保持 odd voter invariant
- 没有让 learner 在 promote 前进入 quorum
- 没有让 partial promote 被恢复成 committed membership
- 没有伪造 4-voter committed membership

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_raft_snapshot_restart ) 9>/tmp/cqupt_raft_build.lock > tmp/test-logs/t081-build.log 2>&1`
- 测试命令：`ctest --preset debug-tests -R "RaftSnapshotRestartTest\.(FollowerKeepsStateAfterInstallSnapshotAndRestart|FullClusterRestartsAfterSnapshotAndContinuesWriting|RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership)" --output-on-failure > tmp/test-logs/t081-ctest.log 2>&1`
- 脚本命令：Not run
- 文件存在性检查：已确认 `tests/test_raft_snapshot_restart.cpp`、`specs/009-local-rpc-object-storage-stabilization/task-reports/t081-add-restart-recovery-for-committed-batch-membership.md`、`specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md` 存在且已更新
- 结果：`FAIL`
- 失败摘要：
  - `test_raft_snapshot_restart` 定向构建通过
  - 现有回归用例 `RaftSnapshotRestartTest.FollowerKeepsStateAfterInstallSnapshotAndRestart`、`RaftSnapshotRestartTest.FullClusterRestartsAfterSnapshotAndContinuesWriting` 继续 `PASS`
  - 新增用例 `RaftSnapshotRestartTest.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership` 按预期暴露生产缺口：第二个 learner 在 first learner 仅 `ready_to_promote / waiting_for_pair` 时仍被 `pending AddLearner proposal already exists` 阻塞，系统无法形成真实 `3 voters + 2 ready learners -> committed 5 voters`，因此 restart 后无法验证 `5 voters / quorum 3`
  - 失败断言：
    - `second_join_response.summary().code()` 实际为 `7`，期望 `raft::METADATA_STATUS_CODE_OK`
    - `second_join_response.disposition()` 实际为 `4`，期望 `raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT`
  - 当前已锁定的 restart safety 语义仍成立：restart 后 committed membership 仍是 `3 voters`、quorum 仍是 `2`、未出现 committed `4 voters`、learners 未被恢复为 voters、blocked/partial promote 未被恢复成 committed membership
- 完整日志路径：
  - `tmp/test-logs/t081-build.log`
  - `tmp/test-logs/t081-ctest.log`

## Build Lock

- `PASS`，成功获取 `/tmp/cqupt_raft_build.lock`

## Platform Notes

- Linux：已完成定向构建与定向 CTest；构建通过，回归用例通过，新 T081 用例按测试先行预期失败并暴露缺口
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前 snapshot restart harness 可以验证 blocked / partial promote 不会在 restart 后变成 committed membership，但还不能表达真实 committed 5-voter batch membership 的恢复。
- 当前运行时缺少 first-class committed membership history / durable config trace，restart 后只能通过 committed quorum summary、runtime summary 和 JoinMetadataCluster diagnostics 观察状态。
- 已同步到 `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`。

## Result

- 最终状态：`FAIL`
- 是否可以进入下一任务：可以进入 `T082`，但 `T081` 不能勾选完成
- 阻塞原因：当前生产代码仍没有真实 committed batch membership / batch promote / joint consensus / committed membership persistence-recovery 路径，restart harness 无法观察到真实 `5-voter committed membership` 恢复，只能锁定 blocked/partial promote 的 restart safety 边界
