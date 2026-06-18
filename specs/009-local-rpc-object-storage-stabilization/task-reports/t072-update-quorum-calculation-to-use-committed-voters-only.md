# T072 Update Quorum Calculation To Use Committed Voters Only

## Scope

- 任务类型：实现
- 在 `modules/raft/node/raft_node.cpp` 将 quorum calculation 收口为 committed voters only。
- 保证 learner 不参与 commit majority、election majority、quorum 判断。
- 不实现 learner AppendEntries catch-up。
- 不实现 learner InstallSnapshot catch-up。
- 不实现 promote-to-voter。
- 不实现 batch promote / joint consensus。

## Task Source

- `tasks.md`: T072
- `plan.md`
- `data-model.md`
- `contracts/metadata-learner-join.md`

## Files Changed

- `modules/raft/node/raft_node.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t072-update-quorum-calculation-to-use-committed-voters-only.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- 未修改 proto
- 未修改 example 脚本
- 未修改 CMake

## What Changed

- 在 `raft_node.cpp` 新增 committed voter helper：
  - 去重后的 committed voter peer 列表
  - committed voter id 列表
  - committed voter count
  - committed voter quorum size
  - 按 committed voters 统计 replicated voter 数
- 将以下路径统一改为基于 committed voters 计算：
  - 启动日志中的 quorum 诊断
  - `GetCommittedMembershipQuorumSummary()`
  - `BuildRuntimeMembershipSummaryLocked()`
  - `StartElection()` 的 election majority
  - `ReplicateLogEntryToMajority()` 的 commit majority
  - `AdvanceCommitIndexUnlocked()` 的 commit majority
- 实现显式排除 runtime learners：pending learner / observed learner 都不会进入 committed voter 集合，也不会改变 quorum。

## Boundary Checks

- 没有实现 learner AppendEntries catch-up
- 没有实现 learner InstallSnapshot catch-up
- 没有实现 promote-to-voter
- 没有实现 batch promote / joint consensus
- 没有修改 committed membership change 语义
- 没有把 ViewNode 当成 membership authority
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 保持 committed membership authority 仍由 Raft 决定

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum test_raft_election test_raft_log_replication > tmp/test-logs/t072-build.log 2>&1 ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|RaftElection|RaftLogReplication|LogReplication" --output-on-failure > tmp/test-logs/t072-ctest.log 2>&1`
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorumTest.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet" --output-on-failure > tmp/test-logs/t072-repro-t052.log 2>&1`
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|RaftElection|RaftLogReplication|LogReplication" --output-on-failure > tmp/test-logs/t072-ctest-rerun.log 2>&1`
- 脚本命令：`Not run`
- 文件存在性检查：`test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t072-update-quorum-calculation-to-use-committed-voters-only.md`
- 结果：`PASS`
- 失败摘要：
  - 首轮 targeted CTest 出现一次 `IntegratedObjectStorageQuorumTest.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet` 时序性失败
  - 单独复现该 case 通过
  - 完整 targeted CTest 重跑通过
- 完整日志路径：
  - `tmp/test-logs/t072-build.log`
  - `tmp/test-logs/t072-ctest.log`
  - `tmp/test-logs/t072-repro-t052.log`
  - `tmp/test-logs/t072-ctest-rerun.log`

## Build Lock

- 使用 `flock` 构建锁
- 已获得锁
- build/test 未跳过

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前 quorum 生产路径已经显式只看 committed voters，但 learner catch-up / promote 仍未实现；后续由 T073 及后续任务继续补完 learner 角色约束与 promote 安全。
- 首轮 targeted CTest 有一次旧用例时序性抖动，最终完整 targeted CTest 已通过；如果后续再次出现，应单独跟踪该集成用例的时序稳定性，而不是放宽 learner/quorum 断言。

## Result

- 最终状态：`PASS`
- 可以进入下一任务
- 下一步可进入 T073
