# T018 Add Deterministic Self Refresh Beyond TTL Test

## Scope

本任务是 Phase 3 ViewNode self refresh 的测试先行任务。

- 只在 `tests/view_node_discovery_test.cpp` 新增 deterministic test
- 把“ViewNode 只启动时注册一次、超过 TTL 后自身掉成 stale/suspect/dead”的问题固定成可重复测试

本任务不实现 ViewNode self refresh 生产逻辑，不修改 ViewNode app loop，不改 peer sync，不改 Raft membership。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T018
- `specs/009-local-rpc-object-storage-stabilization/contracts/identity-lifecycle.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `tests/view_node_discovery_test.cpp`
- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`

## Files Changed

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t018-add-deterministic-self-refresh-beyond-ttl-test.md`

未修改：

- ViewNode registry 生产实现
- ViewNode app self refresh loop
- StorageNode 代码
- MetadataNode 代码
- Raft membership / quorum
- proto
- example 脚本

## What Changed

- 新增 deterministic 测试：
  - `ViewNodeDiscoveryTest.ViewNodeSelfRefreshKeepsSelfLiveBeyondDeadTtl`
- 测试使用 `RunningViewNodeDiscoveryService` 现有 fake clock 注入能力，通过 `set_now_unix_ms(...)` 显式推进时间。
- 测试流程：
  - 先注册一个 ViewNode self record
  - 在 `now=100` 时确认初始 cluster view 中 self record 为 `LIVE`
  - 把服务时间直接推进到 `now=191`，超过 `dead_timeout=90ms`
  - 断言 cluster view 里 self record 仍存在、liveness 仍为 `LIVE`
  - 进一步要求 `last_seen_unix_ms > 100` 且 `last_sequence > 0`，避免“只靠更晚查询时间伪装成 live”这种弱实现

## Boundary Checks

- 没有实现 self refresh 生产逻辑。
- 没有实现 ViewNode peer sync。
- 没有把 ViewNode 变成 Raft membership authority。
- 没有让 `observed_time` 单独成为 merge authority。
- 没有引入真实 `sleep`。
- 没有默认全量构建。

## Validation

- 构建命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：`PASS`
- 构建日志：`tmp/test-logs/t018-build.log`

- 测试命令：

```bash
(
  flock -n 9 || exit 99
  ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：`FAIL`
- 失败测试：
  - `ViewNodeDiscoveryTest.ViewNodeSelfRefreshKeepsSelfLiveBeyondDeadTtl`
- 关键断言：
  - `cluster_result.result.snapshot.view_nodes.size()` 期望为 `1`，实际为 `0`
- 失败分类：
  - 新测试成功暴露当前生产实现缺口：ViewNode 没有持续 self refresh，超过 dead TTL 后 self record 被 cluster view 过滤掉
- 末尾日志摘要：
  - `Expected equality of these values: cluster_result.result.snapshot.view_nodes.size() Which is: 0  1U Which is: 1`
- 完整日志：
  - `tmp/test-logs/t018-ctest.log`

## Build Lock

- build/test 均使用了 `flock` 构建锁。
- 两次都成功获得锁，没有因为锁竞争跳过验证。

## Platform Notes

- Linux：已执行 targeted build/test；build 通过，测试按预期暴露生产缺口。
- Windows：pending。
- macOS：pending。

## Risks / Follow-ups

- 当前失败说明 Phase 3 的真实缺口已经被 deterministic 化：ViewNode 仅在启动时注册一次还不够，后续必须有 self refresh 持续推进自身 observed state。
- 后续实现任务需要补齐：
  - ViewNode self refresh 状态更新路径
  - app 层 self refresh loop
  - self refresh payload 中的 sequence / incarnation 语义
- 本测试已经要求 `last_seen_unix_ms` 和 `last_sequence` 真实前进，因此后续不能用“查询时强行把自身判 live”这种弱实现混过去。

## Result

- 最终状态：`PASS`
- T018 作为测试先行任务已完成：测试已新增，并且 deterministic 地暴露了当前生产实现缺口。
- 可以进入 T019 / 后续实现任务；其中真正修复 self refresh 生产逻辑需要后续实现任务完成。
