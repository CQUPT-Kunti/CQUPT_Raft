# T057 Add Duplicate Join And Pending Membership Change Rejection Tests

## Scope

本任务在 `tests/integrated_object_storage_quorum_test.cpp` 增加 Metadata dynamic join 的 quorum 安全边界测试。

- 只新增测试
- 不修改 JoinMetadataCluster 生产实现
- 不修改 Raft membership / quorum / learner replication / promote

## Task Source

- `tasks.md`: T057
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `tests/integrated_object_storage_quorum_test.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `modules/view/view_registry.h`

## Files Changed

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t057-add-duplicate-join-and-pending-membership-change-rejection-tests.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

在 `tests/integrated_object_storage_quorum_test.cpp` 新增：

- `IntegratedObjectStorageQuorumTest.DuplicateObservedJoinCandidateDoesNotCreateDuplicateCommittedMembershipEntry`
- `IntegratedObjectStorageQuorumTest.PendingObservedJoinCandidatesDoNotPolluteCommittedMembershipOrQuorum`

并补充了一个只读测试 helper：

- `DescribeCommittedMembershipSummary(...)`
- `ExpectCommittedMembershipUnchangedOnRunningNodes(...)`

测试策略基于当前仓库已存在的能力边界：

- 用 `ViewNodeRegistry` 注入 Metadata candidate 的 observed registration
- 用 `RaftNode::GetCommittedMembershipQuorumSummary()` 验证 committed membership 仍然只包含真实 3 个 voter
- 在停掉 1 个 committed voter 后继续执行 metadata create/commit，证明 duplicate / pending join 不会把 quorum 从 2 错抬高，也不会让 candidate 本地变成 voter

说明：

- 当前仓库里还没有可直接驱动的 `JoinMetadataCluster` / pending membership change 生产接口
- 因此本任务测试落在“未完成 join API 之前，observed candidate 输入不能污染 committed membership”这一安全边界
- duplicate candidate 的第二次 observed register 当前返回 `kIdempotentReplay`，测试将其视为“明确幂等语义”，不是重复 membership entry

## Boundary Checks

- 未修改生产代码
- 未修改 proto
- 未修改 CMake
- 未修改 Raft membership / quorum
- 未实现 learner catch-up / promote-to-voter
- 未把 ViewNode observed registration 当成 membership authority

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "DuplicateObservedJoinCandidate|PendingObservedJoinCandidates" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Summary:
  - `IntegratedObjectStorageQuorum`: `5/5` PASS
  - new T057 tests: `2/2` PASS
- Complete logs:
  - `tmp/test-logs/t057-build.log`
  - `tmp/test-logs/t057-new-tests.log`
  - `tmp/test-logs/t057-ctest.log`

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes for final build/test validation
- Earlier during development there was one transient lock miss; final validation was rerun under the lock and passed

## Platform Notes

- Linux: targeted build/test validated
- Windows: not run, pending
- macOS: not run, pending

## Risks / Follow-ups

- 当前测试证明的是“observed candidate / duplicate observed join 不会污染 committed membership”这一安全边界
- 真正的 leader-side `JoinMetadataCluster` duplicate / pending rejection 仍需要后续任务在生产 API 可用后继续补齐
- learner replication、pending membership state machine、promote-to-voter 仍未在本任务实现

## Result

PASS

- 已满足 T057 的测试任务边界
- 可以进入后续 Metadata join 实现/验证任务
