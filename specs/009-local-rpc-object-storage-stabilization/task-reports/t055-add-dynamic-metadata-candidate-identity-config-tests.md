# T055 Add Dynamic Metadata Candidate Identity Config Tests

## Scope

本任务只补 `dynamic MetadataNode candidate` 的 identity/config 测试，不实现 JoinMetadataCluster、AddLearner、learner catch-up、promote-to-voter，也不修改生产 membership / quorum 逻辑。

## Files Changed

- `tests/cluster_config_test.cpp`
- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t055-add-dynamic-metadata-candidate-identity-config-tests.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### `tests/node_identity_test.cpp`

新增：

- `NodeIdentityTest.T055MetadataDynamicJoinCandidateCannotPersistLocalVoterMembershipState`
- `NodeIdentityTest.T055MetadataDynamicJoinCandidateReloadAsCommittedVoterFailsOnMembershipStateMismatch`

覆盖语义：

- dynamic Metadata candidate 可以有本地 identity，但 `source=explicit_override` 的本地 identity 不能持久化成 `membership_state=voter`。
- 已经持久化为 candidate 的 identity，不能在 reload 时通过本地期望把自己解释成 committed voter。
- candidate 与 bootstrap voter 仍然保持分离：bootstrap voter 走 `source=config_generator + voter`，dynamic candidate 走 `source=explicit_override + candidate`。

### `tests/cluster_config_test.cpp`

更新：

- `cluster_config_validation_test.allows_metadata_candidate_outside_initial_membership_and_roundtrips_json`
- 新增 `cluster_config_validation_test.rejects_metadata_candidate_that_attempts_local_voter_role`

覆盖语义：

- metadata candidate config 允许存在于 `metadata_nodes[]`，并保留自己的 `raft_id` 与 `initial_role=candidate`。
- candidate 的 `raft_id` 不进入 `initial_raft_membership.voter_raft_ids`。
- `ComputeInitialRaftQuorum(...)` 仍只按初始 3 个 voter 计算 quorum，candidate 不会通过本地 config 影响 voter quorum。
- 如果把 candidate 本地塞进 `initial_raft_membership.voter_raft_ids`，配置校验必须失败。

## How This Proves Candidate Cannot Become Voter Locally

- identity 路径：
  - 如果 dynamic candidate 直接持久化 `membership_state=voter`，`LoadOrCreateNodeIdentity(...)` 返回 `kInvalidArgument`，并给出 `InvalidMembershipState` 诊断。
  - 如果 candidate 文件已经存在，再用 `ExpectedNodeIdentity.membership_state=voter` 去重载，会得到 `MembershipStateMismatch`，不能把本地 candidate 文件解释成 committed voter。
- config 路径：
  - candidate config 可以被解析和 round-trip，但不进入 `initial_raft_membership.voter_raft_ids`。
  - 如果尝试把 candidate 的 `raft_id` 塞进 voter membership，`ValidateClusterConfig(...)` 返回 `InvalidInitialMembership`。

这两条一起锁定：dynamic Metadata candidate 只能是本地 joining/candidate 身份，真正 learner/voter authority 仍属于 Metadata leader 和 committed Raft membership log，而不是本地 config、identity_file 或 ViewNode observation。

## Validation

Build:

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target cluster_config_test test_node_identity > tmp/test-logs/t055-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

Test:

```bash
ctest --preset debug-tests -R '(^cluster_config_|^NodeIdentityTest\.)' --output-on-failure > tmp/test-logs/t055-ctest.log 2>&1
```

## Validation Result

- PASS
- Build target:
  - `cluster_config_test`
  - `test_node_identity`
- Tests:
  - `NodeIdentityTest.T055MetadataDynamicJoinCandidateCannotPersistLocalVoterMembershipState`: PASS
  - `NodeIdentityTest.T055MetadataDynamicJoinCandidateReloadAsCommittedVoterFailsOnMembershipStateMismatch`: PASS
  - `cluster_config_validation_test.rejects_metadata_candidate_that_attempts_local_voter_role`: PASS
  - `cluster_config_validation_test.allows_metadata_candidate_outside_initial_membership_and_roundtrips_json`: PASS
  - 相关 `NodeIdentityTest.*` 与 `cluster_config_*` 既有用例未退化，全部通过
- Logs:
  - `tmp/test-logs/t055-build.log`
  - `tmp/test-logs/t055-ctest.log`

## Result

- PASS
- 已在 `tasks.md` 中只勾选 T055。
- 可以进入 T056 / T061 等后续 Metadata dynamic join 任务。
