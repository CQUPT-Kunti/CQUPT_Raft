# T073 Exclude Learners From RequestVote And Candidacy Paths

## Scope

本任务在 `modules/raft/node/raft_node.cpp` 落地 learner election safety：把 learner / non-voter 角色排除出 `RequestVote`、candidacy 和 leader election 路径。

本任务不实现：

- learner AppendEntries catch-up
- learner InstallSnapshot catch-up
- promote-to-voter
- batch promote / joint consensus
- committed membership change

## Task Source

- `tasks.md`: T073
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/test_raft_election.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t071-extend-runtime-membership-representation-for-voters-and-learners.md`

## Files Changed

- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/test_raft_election.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t073-exclude-learners-from-requestvote-and-candidacy-paths.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### Local learner role hint

`RaftNode::ValidateNodeIdentity()` 现在除了维护旧的 `node_identity.txt` 之外，还会在同一 `data_dir` 下读取结构化 `node.identity`：

- `membership_state=voter` -> `RuntimeMembershipRole::kVoter`
- `membership_state=learner` -> `RuntimeMembershipRole::kLearner`
- `membership_state=joining/candidate/non_raft` -> `RuntimeMembershipRole::kNonMember`

这个 role hint 只用于 runtime election safety，不改变 committed membership 持久化语义。

### Runtime membership

在 T071 的 runtime membership 基础上继续收口：

- 本地 role hint 是 `learner` / `non_member` 时，不把本地节点塞进 runtime voter set
- 本地 role hint 是 `learner` 时，会在 runtime summary 中显式表现为 learner entry
- `committed_voter_quorum_size` 继续只按 runtime voter 数量计算

### RequestVote / candidacy guard

在 `StartElection()` 中新增 guard：

- 本地 runtime role 不是 `voter` 时，直接跳过选举
- 不进入 `Candidate`
- 不发 `RequestVote`
- 不会因为 `quorum <= 1` 走自选 leader 路径

在 `OnRequestVote()` 中新增 guard：

- 本地 runtime role 不是 `voter` 时，始终 `vote_granted=false`
- 高 term 仍可把本地 term 推进到更新 term 并保持 follower
- learner 不会给其他节点提供 voter majority

在 `OnElectionWon()` 中新增 guard：

- 即使进入该路径，本地 runtime role 不是 `voter` 时也拒绝转成 `Leader`

## Boundary Checks

- 未实现 learner catch-up
- 未实现 learner snapshot catch-up
- 未实现 promote-to-voter
- 未实现 batch promote
- 未修改 committed membership change 语义
- 未把 ViewNode observation 当成 election membership 输入
- 正常 voter election 路径未退化

## Test Updates

在 `tests/test_raft_election.cpp` 新增：

- `RaftElectionTest.LearnerIdentityNodeRejectsVoteRequestsAndCannotSelfElectLeader`

测试做法：

1. 先在本地 `data_dir` 写入 `membership_state=learner` 的 `node.identity`
2. 构造一个无 peers 的 learner 节点并启动
3. 验证它不会自选 leader
4. 显式调用 `OnRequestVote(...)`
5. 验证 `vote_granted=false`
6. 验证它仍然不是 `Candidate` / `Leader`

原有测试继续覆盖：

- pending learner 不进入 `RequestVote` 统计
- 3 voters + 1 learner 的 election / quorum 语义不退化

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_raft_election integrated_object_storage_quorum
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `ctest --preset debug-tests -R "RaftElection|IntegratedObjectStorageQuorum" --output-on-failure`
  - Result: PASS
- Summary:
  - `RaftElectionTest.*`: `4/4` PASS
  - `IntegratedObjectStorageQuorumTest.*`: `10/10` PASS

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated
- Windows: not run, pending
- macOS: not run, pending

## Risks / Follow-ups

- 当前 learner role hint 依赖本地 `node.identity` 的 `membership_state`，还没有完整落到 committed membership log 恢复路径
- 当前 election guard 已经把 learner 排除出投票/参选，但真正的 learner replication progress tracking 仍留给 T074/T075
- promote-to-voter、odd voter 扩容和 batch promote 仍留给后续任务

## Result

PASS

- learner 已被排除出 `RequestVote`
- learner 已被排除出 candidacy / leader election
- election majority 仍只使用 committed voters
- 没有提前实现 catch-up / promote / batch promote
- 可以进入 T074
