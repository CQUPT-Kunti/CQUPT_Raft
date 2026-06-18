# T071 Extend Runtime Membership Representation For Voters And Learners

## Scope

本任务在 `modules/raft/node/raft_node.h` 和 `modules/raft/node/raft_node.cpp` 扩展 `RaftNode` 的 runtime membership 表达，显式区分 committed voters 和 runtime learners，为后续 T072/T073/T074 提供基础。

本任务不实现：

- committed-voters-only quorum calculation 变更
- learner AppendEntries catch-up 生产路径
- learner InstallSnapshot catch-up 生产路径
- promote-to-voter
- batch promote / joint consensus
- election policy 改动

## Task Source

- `tasks.md`: T071
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/test_raft_election.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `tests/test_raft_log_replication.cpp`

## Files Changed

- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/test_raft_election.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t071-extend-runtime-membership-representation-for-voters-and-learners.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### Runtime membership API

在 `raft_node.h` 新增：

- `RuntimeMembershipRole`
- `RuntimeMembershipEntry`
- `RuntimeMembershipSummary`
- `GetRuntimeMembershipSummary() const`

这组类型把 runtime membership 明确拆成：

- `voter_ids` / `voter_entries`
- `learner_ids` / `learner_entries`
- `committed_voter_quorum_size`
- `local_role`

### Runtime membership projection

在 `raft_node.cpp` 新增 `BuildRuntimeMembershipSummaryLocked()`，它现在按以下边界构造 runtime membership：

- committed voters 仍然只来自 `config_.node_id + config_.peers`
- runtime learners 当前只来自 `pending_add_learner_proposal_`
- learner 条目带有：
  - `raft_id`
  - `canonical_node_id`
  - `candidate_incarnation_id`
  - `candidate_sequence`
  - `persistent_generation`
  - `data_dir_fingerprint`
  - `pending=true`
  - `committed=false`

### Duplicate / conflict behavior

- committed voters 用 `raft_id` 去重，避免 duplicate voter entry
- pending learner 如果 `raft_id` 与 committed voter 冲突，不会进入 learner set
- `ProposeAddLearner(...)` 原有 duplicate / conflicting / pending 语义保持不变
- runtime summary 会反映：
  - accepted learner 只有一条
  - duplicate learner replay 不新增第二条
  - conflicting / pending request 不污染已有 runtime learner 表达

## Boundary Checks

- 未修改 committed membership 语义
- 未修改当前 quorum calculation 行为
- 未实现 T072 committed-voters-only quorum calculation 收口
- 未实现 learner AppendEntries catch-up 生产逻辑
- 未实现 learner InstallSnapshot catch-up 生产逻辑
- 未实现 promote-to-voter
- 未修改 election policy
- 未让 learner 默认进入 voter set
- 未让 ViewNode 成为 membership authority

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_raft_election integrated_object_storage_quorum test_raft_log_replication
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `ctest --preset debug-tests -R "RaftElection|IntegratedObjectStorageQuorum|RaftLogReplication|LogReplication" --output-on-failure`
  - Result: PASS
- Summary:
  - `RaftElectionTest.*`: `3/3` PASS
  - `RaftLogReplicationTest.*`: `3/3` PASS
  - `IntegratedObjectStorageQuorumTest.*`: `10/10` PASS

## Test Updates

### `tests/test_raft_election.cpp`

保留 T068 的 election safety 断言，并新增 runtime membership 断言：

- accepted learner 后 runtime summary 仍是 `3 voters + 1 learner`
- learner 条目 `pending=true`
- learner 条目 `committed=false`
- learner 不会进入 `voter_ids`

### `tests/integrated_object_storage_quorum_test.cpp`

在 `AddLearnerProposalPathRejectsFollowerAndPreservesDuplicatePendingBoundary` 中补充 runtime membership 断言：

- accepted learner 后 leader runtime summary 出现单个 learner
- duplicate replay 后 learner 条目仍然只有一条
- conflicting / pending request 不会新增第二条 learner

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated
- Windows: not run, pending
- macOS: not run, pending

## Risks / Follow-ups

- 当前 runtime learner 仍然是“pending AddLearner projection”，不是已提交 learner membership
- 当前 quorum / election / commit 仍然按 committed voters 行为运行；真正的 committed-voters-only 计算收口留给 T072
- 当前 learner progress tracking、AppendEntries catch-up、InstallSnapshot catch-up 仍留给 T073/T074/T075
- 当前 promote safety / odd voter 批量扩容仍留给后续阶段

## Result

PASS

- runtime membership 现在可以明确区分 voters 和 learners
- learner 不会默认进入 voter set
- duplicate / conflicting learner state 不会产生重复 runtime membership entry
- 没有提前实现 T072 quorum calculation、catch-up 或 promote
- 可以进入 T072
