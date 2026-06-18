# T060 Implement Metadata Leader Join Validation

## Scope

本任务在 `MetadataServiceImpl` 落地 `JoinMetadataCluster` 的 leader-side validation，只做 authority 校验、candidate 校验、duplicate/pending/conflict 拒绝语义，不实现 AddLearner、learner catch-up、promote-to-voter 或任何 committed membership change。

## Task Source

- `tasks.md`: T060
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `proto/metadata.proto`
- `modules/raft/service/metadata_service_impl.h`
- `modules/raft/service/metadata_service_impl.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t056-add-joinmetadatacluster-leader-validation-tests.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t057-add-duplicate-join-and-pending-membership-change-rejection-tests.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t059-add-additive-joinmetadatacluster-request-response-contract.md`

## Files Changed

- `modules/raft/service/metadata_service_impl.h`
- `modules/raft/service/metadata_service_impl.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t060-implement-metadata-leader-join-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### Metadata service

在 `MetadataServiceImpl` 新增 `JoinMetadataCluster(...)` RPC 实现，并引入 service-local 的轻量 pending validation 状态：

- 只有当前节点 `status.role == "Leader"` 时才允许进入 join validation
- follower / non-leader 返回：
  - `summary.code = NOT_LEADER`
  - `disposition = NOT_LEADER`
  - `leader_hint`
- leader 只做 validation，不改 Raft committed membership
- validation 成功返回：
  - `summary.code = OK`
  - `disposition = ACCEPTED_PENDING_COMMIT`
  - `requested_membership = LEARNER`
  - `committed_membership_changed = false`
- 同一 pending candidate 的重复请求返回：
  - `summary.code = IDEMPOTENT_REPLAY`
  - `disposition = DUPLICATE`
- 已存在 pending candidate 时：
  - 新 candidate 返回 `PENDING_MEMBERSHIP_CHANGE`
  - 同 identity 但不同事实的冲突请求返回 `REJECTED`
- 无效 candidate 返回：
  - `summary.code = INVALID_ARGUMENT`
  - `disposition = INVALID_CANDIDATE`

### Candidate validation

当前 leader validation 校验：

- `request_id`
- `cluster_id`
- `node_id`
- `candidate_raft_id > 0`
- `candidate_client_address`
- `candidate_raft_address`
- `candidate_incarnation_id`
- `candidate_sequence > 0`
- `persistent_generation > 0`
- `data_dir_fingerprint`
- `local_state_hint` 只能是 `JOINING` / `CANDIDATE`
- `candidate_raft_id` 不能与当前 committed voter set 冲突

### Tests

在 `tests/integrated_object_storage_quorum_test.cpp` 新增：

- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterFollowerRejectsAuthorityAndReturnsLeaderHint`
- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterLeaderValidatesInvalidDuplicateAndPendingWithoutChangingCommittedMembership`

并补了两个 RPC helper：

- `MakeJoinMetadataClusterRequest(...)`
- `JoinMetadataClusterViaAddress(...)`

## Leader vs Follower Authority

- follower / non-leader 不接受 join authority
- 返回 `NOT_LEADER` 与 `leader_hint`
- ViewNode observation 没有被用来绕过 leader authority
- 只有 Metadata leader 能返回 validation-passed

## Invalid / Duplicate / Pending Handling

- invalid candidate：缺字段、非法 `candidate_raft_id`、非法 `local_state_hint`、与 committed voter `raft_id` 冲突时直接拒绝
- duplicate candidate：同一 pending candidate 重放返回 `IDEMPOTENT_REPLAY + DUPLICATE`
- pending membership change：已有 pending candidate 时，新的不同 candidate 返回 `PENDING_MEMBERSHIP_CHANGE`
- conflicting candidate：与 pending candidate 的 node_id / raft_id / endpoint / fingerprint 冲突时返回 `REJECTED`

## Boundary Checks

- 未实现 AddLearner
- 未实现 learner catch-up
- 未实现 promote-to-voter
- 未修改 committed Raft membership
- 未修改 Raft quorum / election
- 未让 ViewNode 成为 membership authority
- validation 状态只保存在 service-local 内存，不写 Metadata state machine，不写 Raft log

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario integrated_object_storage_quorum
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "MetadataClientScenario|IntegratedObjectStorageQuorum" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Summary:
  - `MetadataClientScenario`: `11/11` PASS
  - `IntegratedObjectStorageQuorum`: `7/7` PASS
  - new T060 join validation tests: `2/2` PASS
- Complete logs:
  - `tmp/test-logs/t060-build.log`
  - `tmp/test-logs/t060-ctest.log`

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated
- Windows: not run, pending
- macOS: not run, pending

## Risks / Follow-ups

- 当前 pending join 状态是 service-local 内存状态，不是 committed membership 恢复能力
- leader failover 后 pending validation 状态不会跨 leader 保留；这属于后续真正 membership change / recovery 任务范围
- 当前 `accepted_pending_commit` 只表示 leader validation 通过，不表示已经提案 AddLearner，更不表示已成为 learner/voter

## Result

PASS

- 已满足 T060 的 leader validation 实现边界
- 可以进入 T061
