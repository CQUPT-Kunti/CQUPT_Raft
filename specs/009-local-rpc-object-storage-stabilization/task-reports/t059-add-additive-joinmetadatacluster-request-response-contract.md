# T059 Add Additive JoinMetadataCluster Request Response Contract

## Scope

本任务只做 `JoinMetadataCluster` 的 additive proto contract 和最小编译适配，不实现 Metadata leader validation、AddLearner、learner catch-up、promote-to-voter，也不修改 Raft membership / quorum 逻辑。

## Files Changed

- `proto/metadata.proto`
- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t059-add-additive-joinmetadatacluster-request-response-contract.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Where The Contract Lives

`JoinMetadataCluster` 放在 `proto/metadata.proto` 的 `MetadataService`。

原因：

- 这是 Metadata leader authority 路径，属于 metadata 业务 RPC，而不是 Raft 共识底层 RPC。
- `common.proto` 已经提供 `MetadataResponseSummary`、`MetadataStatusCode`、`MetadataLeaderHint`，适合复用 `NOT_LEADER` 和 leader hint 语义。
- 这样可以保持 proto 边界清晰：
  - `raft.proto` 继续只承载 `RequestVote` / `AppendEntries` / `InstallSnapshot`
  - `metadata.proto` 承载 metadata 写读 RPC 与 dynamic join contract

## What The Request/Response Can Express

### Request

新增 `JoinMetadataClusterRequest`，当前可表达：

- `request_id`
- `cluster_id`
- `node_id`
- `candidate_raft_id`
- `candidate_client_address`
- `candidate_raft_address`
- `candidate_incarnation_id`
- `candidate_sequence`
- `persistent_generation`
- `data_dir_fingerprint`
- `local_state_hint`，只允许表达 `joining/candidate` 本地提示，不提供 voter 声明路径
- `observed_view_node_id`
- `observed_time_unix_ms`
- `observed_metadata_endpoint`

这保证后续 leader validation 可以拿到 candidate identity / endpoint / observed facts，但不会把 ViewNode observation 升格成 membership authority。

### Response

新增：

- `JoinMetadataClusterDisposition`
- `JoinMetadataTargetMembership`
- `JoinMetadataClusterResponse`

当前 response 可表达：

- `accepted_pending_commit`
- `not_leader`
- `duplicate`
- `pending_membership_change`
- `invalid_candidate`
- `rejected`
- `requested_membership=learner`
- `committed_membership_changed`
- `membership_epoch`
- `canonical_node_id`
- `assigned_raft_id`
- 以及通过 `summary.leader_hint` 返回 leader hint

关键边界：

- `accepted_pending_commit` 不等于直接成为 voter。
- `requested_membership` 只表达 `learner`，不提供 “candidate 直接声明 voter” 的 contract。
- `committed_membership_changed` 单独显式表达“是否已进入 committed membership”，避免把“收到请求/通过初步校验”和“已完成 quorum change”混为一谈。

## Additive Compatibility

本任务保持了 additive proto 兼容：

- 未删除任何已有 message / field / service / RPC
- 未重命名任何已有字段
- 未改动任何已有字段编号
- 新增字段和新 message 使用全新编号
- `MetadataService` 只新增 `JoinMetadataCluster` RPC
- `AddLearner` 仍未暴露，避免超前扩展到 T063

## Minimal Test Adaptation

更新 `tests/metadata_client_scenario_test.cpp`：

- 将原先的
  - `JoinMetadataClusterContractIsNotYetExposedByMetadataServiceProto`
- 改为
  - `JoinMetadataClusterContractIsExposedByMetadataServiceProto`

新增断言覆盖：

- `MetadataService` 现在暴露 `JoinMetadataCluster`
- `AddLearner` 仍未暴露
- `JoinMetadataClusterRequest/Response` descriptor 存在且字段齐全
- response 能表达：
  - `NOT_LEADER`
  - `leader_hint`
  - `requested_membership=LEARNER`
  - `committed_membership_changed=false`
- contract 不把 candidate 直接变成 voter

保留了既有 CLI 测试：

- `UnsupportedJoinMetadataClusterCliDoesNotBypassLeaderAuthority`

这说明本任务只补 contract，没有提前实现客户端命令或 T060 的 leader validation。

## Validation

Build:

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario > tmp/test-logs/t059-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

Test:

```bash
ctest --preset debug-tests -R 'MetadataClientScenario' --output-on-failure > tmp/test-logs/t059-ctest.log 2>&1
```

## Validation Result

- PASS
- Build target:
  - `test_metadata_client_scenario`
- Tests:
  - `MetadataClientScenarioTest.JoinMetadataClusterContractIsExposedByMetadataServiceProto`: PASS
  - `MetadataClientScenarioTest.UnsupportedJoinMetadataClusterCliDoesNotBypassLeaderAuthority`: PASS
  - `MetadataClientScenarioTest.FutureJoinLeaderValidationMustReturnNotLeaderAndLeaderHintForFollowerAuthority`: PASS
  - 其余 `MetadataClientScenarioTest.*` 既有用例未退化，全部通过
- Logs:
  - `tmp/test-logs/t059-build.log`
  - `tmp/test-logs/t059-ctest.log`

## Result

- PASS
- 已在 `tasks.md` 中只勾选 T059。
- 可以进入 T060。
