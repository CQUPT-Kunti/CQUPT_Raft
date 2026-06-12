# T064 Ensure ViewNode Metadata Observations Remain Non-Authoritative

## Scope

本任务收紧 ViewNode metadata observation 与 Raft committed membership 的职责边界：

- ViewNode 只记录 metadata discovery / liveness / diagnostics 事实
- metadata observation 不能成为 voter / learner / membership authority
- JoinMetadataCluster 仍必须经过 Metadata leader validation
- AddLearner 仍必须经过 RaftNode proposal path 边界
- 不实现 learner catch-up
- 不实现 promote-to-voter
- 不修改 quorum 规则

## Files Changed

- `modules/view/view_registry.cpp`
- `modules/raft/service/metadata_service_impl.cpp`
- `tests/view_node_discovery_test.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t064-ensure-viewnode-metadata-observations-remain-non-authoritative.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### ViewNode registry boundary

在 `modules/view/view_registry.cpp` 中补了三类边界：

1. metadata observation 统一追加 `kNonAuthorityBoundary` 诊断  
   - `LookupNode`
   - `DiscoverMetadata`
   - `GetClusterView`

2. stale / dead / unavailable metadata observation 不再继续提供 leader hint  
   - 非 `LIVE` metadata snapshot 不参与 leader hint 选择
   - 非 `LIVE` 或 `Unavailable` 时清空 `leader_hint`

3. metadata discovery 的 `membership_epoch` 只从 `LIVE` metadata observation 聚合  
   - stale / dead observation 不再污染 discovery 输出中的 membership epoch

### Metadata service boundary

在 `modules/raft/service/metadata_service_impl.cpp` 中，`JoinMetadataCluster` 响应统一追加 authority 诊断：

- `viewnode_observation=discovery_only`
- `join_authority=metadata_leader_committed_membership_only`
- `requested_membership=learner_not_voter`

这保证即使请求带了：

- `observed_view_node_id`
- `observed_metadata_endpoint`
- `observed_time_unix_ms`

它们也只被解释为 discovery hint，不会被解释成 membership authority。

## Safety Boundary

当前语义明确为：

- ViewNode observation 可以显示 `joining` / `learner` / `voter` 这类 observed facts
- 这些 observed facts 不会进入 committed voter set
- 这些 observed facts 不会自动进入 committed learner set
- 这些 observed facts 不会改变 quorum
- follower 收到 `JoinMetadataCluster` 仍返回 `NOT_LEADER`
- leader 即使接受请求，也只到 `AddLearner` pending-commit admission 边界
- 没有提前实现 catch-up / promote

## Tests Updated

### `tests/view_node_discovery_test.cpp`

更新了现有 observation-only 相关断言：

- `ViewNodeDiscoveryTest.DiscoverMetadataReturnsLiveCandidatesAndNewestLeaderHint`
- `ViewNodeDiscoveryTest.MetadataObservedRegistrationRemainsObservationOnlyAndRespectsMergeAndLiveness`
- `ViewNodeDiscoveryTest.ClusterViewCanExcludeDeadNodesAndEmitWarnings`

补充验证：

- metadata observation 会产生 `kNonAuthorityBoundary` 诊断
- stale metadata candidate 在 cluster view 中保持 observed state，但不再被当作 live membership
- dead metadata observation 会降为 `kDown`
- stale/dead metadata observation 不再继续提供 leader hint

### `tests/integrated_object_storage_quorum_test.cpp`

更新了现有 quorum / join 边界断言：

- `IntegratedObjectStorageQuorumTest.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet`
- `IntegratedObjectStorageQuorumTest.DuplicateObservedJoinCandidateDoesNotCreateDuplicateCommittedMembershipEntry`
- `IntegratedObjectStorageQuorumTest.PendingObservedJoinCandidatesDoNotPolluteCommittedMembershipOrQuorum`
- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterFollowerRejectsAuthorityAndReturnsLeaderHint`
- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterLeaderValidatesInvalidDuplicateAndPendingWithoutChangingCommittedMembership`

补充验证：

- cluster view 中能看到 `kNonAuthorityBoundary`
- `JoinMetadataCluster` 响应 message 明确声明 ViewNode observation 是 discovery-only
- accepted join 仍然是 `learner_not_voter`

## Validation

### Build

```bash
cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery integrated_object_storage_quorum test_metadata_client_scenario
```

- PASS

补充重建：

```bash
cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
```

- PASS

### Test

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
```

- PASS

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery|IntegratedObjectStorageQuorum|MetadataClientScenario" --output-on-failure
```

- PASS

## Result

- PASS
- 已在 `tasks.md` 只勾选 `T064`
- 可以进入 `T065`

## Notes

- 本任务没有实现 learner catch-up
- 本任务没有实现 promote-to-voter
- 本任务没有修改 committed voter quorum 规则
- 本任务没有把 ViewNode observation 提升为 membership authority
