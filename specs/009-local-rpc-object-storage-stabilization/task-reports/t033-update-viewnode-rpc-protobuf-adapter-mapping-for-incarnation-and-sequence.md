# T033 任务报告

## 做了什么

本任务补齐了 ViewNode service/client adapter 在 incarnation-aware observed-state 上的双向映射，重点解决两个缺口：

1. client 发送 `HeartbeatNodeRequest` 时，原来无法把 `incarnation_id` 传到 service。
2. client 读取 `ViewNodeSnapshot` 时，原来只恢复了兼容字段 `last_sequence/last_seen_unix_ms`，没有把它们重建回 `observed_state`，也拿不到 `incarnation_id`。

为此，本次做了最小 additive 变更：

- 在 `proto/view.proto` 中补充：
  - `HeartbeatNodeRequest.incarnation_id`
  - `ViewNodeSnapshot.incarnation_id`
  - `ClusterViewWarning.sequence`
- 在 service adapter 中补齐 request/response 映射
- 在 client adapter 中把 proto snapshot 恢复成完整的本地 `ViewNodeSnapshot::observed_state`
- 保留现有 `last_sequence` / `last_seen_unix_ms` / diagnostics message 路径，不破坏既有 status/discovery 使用方式

## 修改了哪些文件

- `proto/view.proto`
- `modules/view/view_service_impl.cpp`
- `modules/view/view_client.cpp`
- `modules/view/module-notes.md`
- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t033-update-viewnode-rpc-protobuf-adapter-mapping-for-incarnation-and-sequence.md`

## service/client adapter 现在如何保留 incarnation-aware 状态

### service 侧

- `HeartbeatNodeRequest` 现在会把 proto 中的 `incarnation_id` 映射到本地 `HeartbeatNodeRequest`
- `FillProtoSnapshot()` 现在会把 registry snapshot 中的 `incarnation_id` 写回 proto `ViewNodeSnapshot`
- warning 输出现在会保留 `sequence`
- T024 的 `self_refresh_state` diagnostics 仍保留，并额外把 `sequence` 结构化写入 warning 字段

### client 侧

- `ViewNodeClient::HeartbeatNode()` 现在会把本地 request 的 `incarnation_id` 写入 proto request
- `FromProtoSnapshot()` 现在会恢复：
  - `snapshot.incarnation_id`
  - `snapshot.observed_state.incarnation_id`
  - `snapshot.observed_state.sequence`
  - `snapshot.observed_state.observed_at_unix_ms`
- `WarningToDiagnostic()` 现在会恢复 warning 里的 `sequence`

这样 service response 和 client DTO 都能保留后续 merge 需要的关键状态：

- `incarnation_id`
- `sequence`
- `observed_at_unix_ms`

同时兼容字段：

- `last_sequence`
- `last_seen_unix_ms`

仍然保持可用。

## 是否保持现有 discovery / status 行为

保持了。

本次没有删除任何旧字段或旧输出路径，只是在已有路径上补齐结构化状态：

- `DiscoverMetadata` / `DiscoverStorage` / `GetClusterView` 仍按原有方式返回 snapshot
- `last_sequence` / `last_seen_unix_ms` 仍保留
- T024 的 self refresh diagnostics message 仍可观察
- 没有把 ViewNode 变成 membership authority
- 没有修改 peer sync、Raft membership 或 example 脚本

## 新增或更新了哪些测试

更新了已有集成测试：

- `ViewNodeDiscoveryTest.IntegrationClusterViewExposesSelfRefreshSequenceLivenessDiagnostics`
  - 现在额外断言 diagnostics 的结构化 `sequence`
  - 现在额外断言 cluster view client snapshot 中的 `observed_state`

- `ViewNodeDiscoveryTest.IntegrationStorageDiscoveryReturnsEndpointAndObservedState`
  - 现在断言 client 解析后的 storage snapshot 也保留 `observed_state`

- `ViewNodeDiscoveryTest.IntegrationHeartbeatRefreshesStateAndRejectsStaleUpdates`
  - 现在断言 heartbeat response / storage discovery response 的 `observed_state`

新增测试：

- `ViewNodeDiscoveryTest.IntegrationHeartbeatAdapterPreservesIncarnationAwareObservedState`
  - 通过 client 发送带 `incarnation_id` 的 ViewNode heartbeat
  - 断言 heartbeat response snapshot 保留 `incarnation + sequence + observed_at`
  - 再发送同 incarnation 下的旧 sequence，断言被 `stale_ignored`
  - 最后通过 `GetClusterView()` 断言 client 仍能观察到正确的 incarnation-aware 状态和 diagnostics sequence

## 验证命令和结果

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery view_node_app
) 9>/tmp/cqupt_raft_build.lock
```

结果：

- PASS

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
```

结果：

- PASS
- `21/21` tests passed

日志：

- `tmp/test-logs/t033-build.log`
- `tmp/test-logs/t033-ctest.log`

## PASS / FAIL / SKIPPED

PASS

本次没有因为构建锁、target 缺失或环境限制跳过。

可选 local RPC status smoke 本次未执行。

## tasks.md

本任务验证通过后，已仅将 `tasks.md` 中的 T033 从 `[ ]` 改为 `[X]`。

## 是否可以进入后续任务

可以。

T033 完成后，后续 T034/T035 以及 peer sync 相关任务已经可以依赖：

- heartbeat request 能携带 `incarnation_id`
- service response / client DTO 能保留 incarnation-aware observed-state
- diagnostics 的 `sequence` 不会在 adapter 层丢失
