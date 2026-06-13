# T077 Record Learner Catch-Up Validation

## Scope

- 任务类型：验证 / 文档
- 本任务汇总 US4 learner catch-up、learner quorum safety、learner promotion-blocking 的已实现边界和 Linux 实际验证记录。
- 同步修正了 `tasks.md` 中 T077 的旧泛化报告文件名引用，但未勾选完成状态。
- 本任务不写生产代码，不改测试逻辑，不实现 promote-to-voter、batch promote 或 joint consensus。

## Task Source

- `tasks.md`: T077
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t066-add-learner-appendentries-catch-up-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t067-add-learner-installsnapshot-catch-up-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t068-add-learner-excluded-from-requestvote-and-leader-election-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t069-add-3-voters-1-learner-quorum-remains-2-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t070-add-single-learner-promote-blocked-by-even-voter-count-test.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t071-extend-runtime-membership-representation-for-voters-and-learners.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t072-update-quorum-calculation-to-use-committed-voters-only.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t073-exclude-learners-from-requestvote-and-candidacy-paths.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t074-enable-learner-log-replication-progress-tracking.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t075-enable-learner-snapshot-install-and-applied-progress-tracking.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t076-add-pending-learner-ready-to-promote-waiting-for-pair-status-reporting.md`

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t077-record-learner-catch-up-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- 未修改生产代码
- 未修改测试代码
- 未修改 proto
- 未修改 CMake

## US4 Learner Catch-Up Summary

- T066：`test_raft_log_replication` 已覆盖 learner AppendEntries catch-up，确认 learner 可以追日志且不会扩大 committed voter quorum。
- T067：`test_raft_snapshot_catchup` 已覆盖 learner InstallSnapshot catch-up 和失败不污染 committed voter membership。
- T068 / T073：`test_raft_election` 已覆盖 learner 被排除出 RequestVote、candidacy 和 leader election。
- T069 / T072：`integrated_object_storage_quorum` 已覆盖 committed-voters-only quorum calculation，确认 `3 voters + 1 learner` 的 quorum 仍为 `2`。
- T070：`integrated_object_storage_quorum` 已覆盖 single learner promote blocked by even voter count。
- T071：runtime membership 已能区分 voters / learners，并把 learner 保持在 non-voter runtime 视图内。
- T074：leader 已能记录 learner log replication progress。
- T075：leader 已能记录 learner snapshot install / applied progress；pending learner 可以通过 snapshot catch-up。
- T076：`JoinMetadataCluster` 诊断已可汇报 `learner_status=pending`、`learner_status=ready_to_promote`、`promotion_status=waiting_for_pair`，但这仍是只读状态汇报，不是 committed membership 变更。

## Current Learner Semantics

- learner 可以作为 non-voter 节点存在。
- learner 可以通过 AppendEntries 追日志。
- learner 可以通过 InstallSnapshot 追 snapshot。
- learner progress 可以被 leader 侧 runtime summary 记录和诊断。
- learner 不参与 commit quorum。
- learner 不参与 election quorum。
- learner 不参与 RequestVote。
- learner 不能成为 candidate / leader。
- `3 voters + 1 learner` 的 quorum 仍为 `2`。
- `1 voter + 1 learner` 不会被当成可用 majority。
- single ready learner 不会被自动 promote 成 `4 voters`。
- ready learner 当前只会被汇报为 `waiting_for_pair` 或等价 blocked 状态。
- committed voter set 仍是 quorum / election / commit 的事实来源。

## Safety Invariants

- learner 不进入 voter set，除非后续显式 promote 任务完成。
- learner catch-up 不等于 promote。
- learner ready-to-promote 不等于 voter。
- learner 不改变 committed voter quorum。
- learner 不降低 quorum。
- learner 不参与 election。
- learner progress failure 不污染 committed membership。
- single learner promote 不会提交成 even voter count。
- ViewNode observation 不会触发 learner promote。
- status reporting 不修改 committed membership。

## Linux Validation

- 当前 T077 尝试执行的构建命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication test_raft_snapshot_catchup integrated_object_storage_quorum test_raft_election ) 9>/tmp/cqupt_raft_build.lock`
- 当前 T077 尝试执行的测试命令：
  - `ctest --preset debug-tests -R "RaftLogReplication|LogReplication|RaftSnapshotCatchup|SnapshotCatchup|IntegratedObjectStorageQuorum|RaftElection" --output-on-failure`
- 当前 T077 结果：`SKIPPED`
- 原因：两次尝试都未获得 `/tmp/cqupt_raft_build.lock`，因此未在本任务重新构建 / 复跑。

- 历史实际 Linux 验证记录：
  - `test_raft_log_replication`
    - 历史结果：`PASS`
    - 依据：T066、T071、T072、T074
    - 日志：`tmp/test-logs/t074-build.log`、`tmp/test-logs/t074-ctest.log`
  - `test_raft_snapshot_catchup`
    - 历史结果：`PASS`
    - 依据：T067、T075
    - 日志：`tmp/test-logs/t067-build.log`、`tmp/test-logs/t067-ctest.log`、`tmp/test-logs/t075-snapshot-tests.log`
  - `integrated_object_storage_quorum`
    - 历史结果：`PASS`
    - 依据：T069、T070、T071、T072、T073、T074、T076
    - 日志：`tmp/test-logs/t076-build.log`、`tmp/test-logs/t076-ctest.log`
  - `test_raft_election`
    - 历史结果：`PASS`
    - 依据：T068、T071、T072、T073
    - 日志：`tmp/test-logs/t072-ctest.log`、`tmp/test-logs/t072-ctest-rerun.log`
  - `metadata_client_scenario`
    - 本任务未重跑
    - 历史结果：`PASS`
    - 依据：T076
    - 日志：`tmp/test-logs/t076-ctest.log`

## Windows Validation

- Windows：`pending / not run`
- 原因：本任务未在 Windows 环境执行，且当前 T077 没有跨平台实机验证入口。

## macOS Validation

- macOS：`pending / not run`
- 原因：本任务未在 macOS 环境执行。

## Skipped Checks

- 当前 T077 的 targeted build：skipped，原因是 build lock 未获取。
- 当前 T077 的 targeted ctest：skipped，原因是 build lock 未获取后不继续伪造测试执行。
- Windows validation：skipped，无 Windows host。
- macOS validation：skipped，无 macOS host。
- local RPC learner join smoke：not run，不在本任务范围。
- promote-to-voter / batch promote / joint consensus：not run，属于后续阶段。

## Remaining Risks / Follow-ups

- promote-to-voter 仍未实现 / 未验证。
- `3 voters + 2 ready learners` 的 batch promote 仍未实现 / 未验证。
- joint consensus 仍未实现 / 未验证。
- odd-voter-safe promotion policy 还需要 Phase 9 的批量 promote 测试继续收口。
- learner 长时追赶、失败恢复、soak 稳定性仍缺少更长时间验证。
- Windows / macOS 仍未验证。
- local RPC runtime dynamic metadata join + learner catch-up smoke 仍缺失。
- multi-ViewNode discovery 对 metadata learner join 的运行时影响仍未完整验证。
- 已同步 `cross-task-risk-notes.md`。

## Validation

- 构建命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication test_raft_snapshot_catchup integrated_object_storage_quorum test_raft_election ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：
  - `ctest --preset debug-tests -R "RaftLogReplication|LogReplication|RaftSnapshotCatchup|SnapshotCatchup|IntegratedObjectStorageQuorum|RaftElection" --output-on-failure`
- 脚本命令：Not run
- 文件存在性检查：
  - `test -f specs/009-local-rpc-object-storage-stabilization/tasks.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t077-record-learner-catch-up-validation.md`
- 结果：`SKIPPED`
- 失败摘要：
  - 构建锁未获取，命令返回 `99`
  - 未执行重新构建与复跑
- 完整日志路径：
  - 当前任务未生成有效 build/test 日志
  - 历史参考日志：`tmp/test-logs/t074-ctest.log`、`tmp/test-logs/t075-snapshot-tests.log`、`tmp/test-logs/t076-ctest.log`

## Build Lock

- 使用了 `flock` 构建锁。
- 未获得锁。
- 因此本任务的 build/test 按规则记为 skipped。

## Platform Notes

- Linux：historical PASS evidence available, current T077 rerun skipped by build lock
- Windows：pending
- macOS：pending

## Result

- 最终状态：`PARTIAL`
- 是否已在 `tasks.md` 只勾选 T077：否
- 是否可以进入下一任务：否
- 阻塞原因：当前 T077 未完成本轮要求的 targeted rerun，且按规则 build lock 未获取时不能把 T077 标记完成
