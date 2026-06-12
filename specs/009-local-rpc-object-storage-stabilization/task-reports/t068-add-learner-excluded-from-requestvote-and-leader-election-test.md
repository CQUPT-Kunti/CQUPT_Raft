# T068 Add Learner Excluded From RequestVote And Leader Election Test

## Scope

本任务只在 `tests/test_raft_election.cpp` 增加 learner election safety 测试，不修改任何生产选举逻辑、不实现 promote-to-voter、不修改 committed membership change。

当前测试覆盖的是“pending learner candidate 在 promote 完成前仍然被排除在 RequestVote / leader election / voter quorum 之外”的边界。

## Task Source

- `tasks.md`: T068
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `tests/AGENTS.md`
- `modules/raft/node/AGENTS.md`
- `tests/test_raft_election.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`

## Files Changed

- `tests/test_raft_election.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t068-add-learner-excluded-from-requestvote-and-leader-election-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

在 `tests/test_raft_election.cpp` 新增：

- `RaftElectionTest.PendingLearnerCandidateIsExcludedFromRequestVoteAndLeaderElectionQuorum`

为避免改生产代码，测试使用一个 fake gRPC `RaftService` 监听 pending learner 的 `candidate_raft_address`，只统计是否收到 `RequestVote`。测试步骤：

1. 启动 3-voter 集群并等待单 leader。
2. 在 leader 上发起 `ProposeAddLearner(...)`，拿到 `kAcceptedPendingCommit`。
3. 断言 `committed_membership_changed == false`。
4. 断言所有运行节点的 committed membership 摘要仍是：
   - `voter_ids = [1,2,3]`
   - `learner_ids = []`
   - `voter_count = 3`
   - `learner_count = 0`
   - `quorum_size = 2`
5. 停掉当前 leader，触发剩余 2 个 voter 重选。
6. 断言新的 leader 只能从剩余 committed voter 中产生。
7. 断言 fake learner endpoint 收到的 `RequestVote` 次数为 `0`。

这证明 promote 未完成前，pending learner candidate 不会被计入选举成员，也不会被当作 voter 拉票。

## Boundary Checks

- 未修改生产代码
- 未修改 election 生产逻辑
- 未实现 promote-to-voter
- 未修改 committed membership change
- 未修改 quorum 生产逻辑
- 未修改 proto
- 未削弱已有 `RaftElection` 测试

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_raft_election
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `ctest --preset debug-tests -R "RaftElection|Election" --output-on-failure`
  - Result: PASS
- Summary:
  - `RaftElectionTest.ThreeNodeClusterElectsExactlyOneLeader`: PASS
  - `RaftElectionTest.FollowerRejectsClientProposeAfterLeaderIsElected`: PASS
  - `RaftElectionTest.PendingLearnerCandidateIsExcludedFromRequestVoteAndLeaderElectionQuorum`: PASS

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated
- Windows: not run, pending
- macOS: not run, pending

## Risks / Follow-ups

- 当前测试验证的是 pending learner candidate 在 promote 前不进入 committed election membership。
- 当前 `RaftNode` 运行时还没有下沉真正的 learner election role；后续 T071-T073 仍需要把 learner membership、quorum 计算和 RequestVote/candidacy 排除正式落到生产实现。
- 本任务没有实现 promote，也没有把 learner 启动成真实 Raft runtime 成员。

## Result

PASS

- learner 在当前已实现边界下不会被计入 committed voter quorum
- pending learner candidate 不会收到 `RequestVote`
- leader re-election 仍然只在 committed voters 内完成
- 可以进入后续任务
