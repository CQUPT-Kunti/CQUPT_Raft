# T062 Add Leader Discovery Through ViewNode Candidates And NOT_LEADER Fallback

## Scope

本任务实现 dynamic MetadataNode candidate 的 leader discovery / fallback：

- 通过 ViewNode `DiscoverMetadata` 获取 MetadataNode candidates
- 优先使用 ViewNode leader hint
- 遇到 `NOT_LEADER` 时按 leader hint 或其余 discovered candidates fallback
- 不实现 AddLearner / learner catch-up / promote-to-voter
- 不修改 committed Raft membership / quorum / election

## Files Changed

- `apps/metadata_node_app.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `tests/CMakeLists.txt`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t062-add-leader-discovery-through-viewnode-candidates-and-not-leader-fallback.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### metadata_node_app dynamic join path

- candidate mode 先对配置中的 ViewNode seeds 发起 `DiscoverMetadata`
- 收集 ViewNode 返回的 `leader_hint.endpoint` 与 `metadata_nodes[*].control_plane_endpoint/endpoint`
- 去重后形成 join target 队列
- 如果 ViewNode discovery 没有返回可用 candidates，才退回到现有 config metadata seed fallback
- 向非 leader 发起 `JoinMetadataCluster` 收到 `NOT_LEADER` 时：
  - 若 response 带 `leader_hint`，优先把 hint 地址加入后续尝试队列
  - 若无 hint，继续尝试剩余 discovered candidates
- fallback 有上限：只对唯一 endpoint 尝试一次，不会无限循环
- request 现在会带上：
  - `observed_view_node_id`
  - 当前尝试 target 的 `observed_metadata_endpoint`

### tests

在 `tests/metadata_client_scenario_test.cpp` 新增了真实 `metadata_node_app` 外部进程场景测试，配合 fake ViewNode / fake MetadataService 覆盖：

1. `MetadataNodeCandidateUsesViewLeaderHintBeforeFollowerFallback`
2. `MetadataNodeCandidateFallsBackToNextDiscoveredMetadataNodeWithoutLeaderHint`
3. `MetadataNodeCandidateReportsClearFailureWhenAllDiscoveredMetadataCandidatesFail`

这些测试证明：

- candidate 不是靠单个硬编码 leader endpoint
- ViewNode leader hint 会被优先消费
- 无 leader hint 时会继续尝试其他 discovered metadata candidates
- 所有 candidates 都失败时会返回明确诊断

## Authority Boundary

- ViewNode 只提供 observed metadata candidates / leader hint
- 真正的 join authority 仍然是 Metadata leader 的 `JoinMetadataCluster`
- 本任务没有提前实现 AddLearner / catch-up / promote
- fallback 成功只到 leader validation / accepted pending commit，仍不会让 candidate 本地变成 voter

## Validation

### Build

```bash
cmake --preset debug-ninja-low-parallel
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target metadata_node_app test_metadata_client_scenario test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock
```

- PASS
- log: `tmp/test-logs/t062-cmake-configure.log`
- log: `tmp/test-logs/t062-build.log`

### Direct T062 test suite

```bash
ctest --preset debug-tests -R "MetadataClientScenario" --output-on-failure
```

- PASS
- log: `tmp/test-logs/t062-metadata-client-only.log`

### Recommended combined suite

```bash
ctest --preset debug-tests -R "MetadataClientScenario|ViewNodeDiscovery" --output-on-failure
```

- FAIL
- T062 新增的 `MetadataClientScenario` 用例全部通过
- 失败项是未触及文件中的现有测试：
  - `ViewNodeDiscoveryTest.MetadataObservedRegistrationRemainsObservationOnlyAndRespectsMergeAndLiveness`
- log: `tmp/test-logs/t062-ctest.log`

## Result

- PASS
- 已在 `tasks.md` 只勾选 `T062`
- 可以进入 `T063`

## Notes

- Linux: 已验证
- Windows/macOS: 本任务未实测，保持 pending
- local RPC startup/status smoke: 本任务未执行，不是本任务核心验证
