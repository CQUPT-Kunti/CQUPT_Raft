# T063 Add AddLearner Proposal Path Stub/Implementation Boundary

## Scope

本任务收口 `RaftNode` 层的 AddLearner proposal path 边界验证：

- 只允许 leader 进入 AddLearner proposal path
- 只提供 admission / duplicate / pending-conflict 边界
- 不提前实现 learner catch-up
- 不提前实现 promote-to-voter
- 不修改 committed voter quorum
- 不把 ViewNode observation 当作 membership authority

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t063-add-addlearner-proposal-path-stub-implementation-boundary.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Current Boundary In RaftNode

当前仓库中的 `modules/raft/node/raft_node.h` / `modules/raft/node/raft_node.cpp` 已具备 T063 所需 AddLearner proposal path 边界：

- `RaftNode::ProposeAddLearner(...)` 作为明确入口
- 先做 request 字段校验，缺字段返回 `kInvalidArgument`
- 非 leader 返回 `kNotLeader`
- committed voter `raft_id` 冲突返回 `kRejected`
- 相同 pending request 返回 `kDuplicate`
- 已有其他 pending membership change 时返回 `kPendingMembershipChange`
- accepted 只到 `kAcceptedPendingCommit`
- `committed_membership_changed` 保持 `false`
- message 明确声明：
  - committed membership log proposal 尚未实现完成
  - learner catch-up 尚未实现
  - promote-to-voter 尚未实现

这满足 T063 的安全边界要求：

- learner 不会直接进入 voter set
- AddLearner 不会伪造 committed membership 成功
- duplicate / pending membership change 不会污染 committed membership
- 当前 committed quorum 摘要仍只基于既有 committed voters 计算

## Membership / Quorum Safety

- `GetCommittedMembershipQuorumSummary()` 仍只从当前 committed voter 集合推导 quorum
- learner 集合在当前阶段保持只读空集，不把 observed/joining candidate 计入 quorum
- `ProposeAddLearner(...)` 不修改 `config_.peers`
- `ProposeAddLearner(...)` 不修改 voter_count / quorum_size
- `ProposeAddLearner(...)` 不写入任何“已完成 learner join”结果

## Test Coverage

本任务直接相关的边界测试已经在仓库中存在并通过：

- `IntegratedObjectStorageQuorumTest.AddLearnerProposalPathRejectsFollowerAndPreservesDuplicatePendingBoundary`
- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterLeaderValidatesInvalidDuplicateAndPendingWithoutChangingCommittedMembership`
- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterFollowerRejectsAuthorityAndReturnsLeaderHint`
- `MetadataClientScenario` 相关 JoinMetadataCluster / leader validation 场景

这些测试证明：

- follower 不能接受 AddLearner authority
- leader admission 不会直接变成 committed membership success
- duplicate request 不会生成重复 learner / membership entry
- pending membership change 存在时不会静默接受冲突请求
- committed membership 与 quorum 不被未完成 join 污染

## Validation

### Configure

```bash
cmake --preset debug-ninja-low-parallel
```

- PASS

### Build

```bash
cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum test_metadata_client_scenario
```

- PASS

### Test

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataClientScenario" --output-on-failure
```

- 首次运行中，`IntegratedObjectStorageQuorumTest.PendingObservedJoinCandidatesDoNotPolluteCommittedMembershipOrQuorum` 因瞬时端口占用中断
- 失败类型：环境级端口冲突，不是 T063 AddLearner 边界断言失败

重跑集成 quorum 套件：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure
```

- PASS
- `IntegratedObjectStorageQuorumTest.AddLearnerProposalPathRejectsFollowerAndPreservesDuplicatePendingBoundary` PASS
- `JoinMetadataCluster` 相关 quorum 场景 PASS

综合判断：

- `MetadataClientScenario` PASS
- `IntegratedObjectStorageQuorum` PASS
- T063 目标边界验证 PASS

## Result

- PASS
- 已在 `tasks.md` 只勾选 `T063`
- 可以进入 `T064`

## Notes

- 本任务未提前实现 learner catch-up
- 本任务未提前实现 promote-to-voter
- 本任务未修改 Raft committed voter quorum 规则
- 本任务未把 ViewNode observation 提升为 membership authority
