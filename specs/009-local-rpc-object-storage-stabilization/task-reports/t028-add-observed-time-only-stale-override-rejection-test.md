# T028 Add Observed-Time-Only Stale Override Rejection Test

## Scope

本任务是 ViewNode observed-state merge / observed_time 边界的测试先行任务。

- 只在 `tests/view_node_discovery_test.cpp` 新增测试
- 验证 `observed_time` 不能单独成为 merge authority

本任务不实现 ViewNode registry 生产 merge 逻辑，不修改 peer sync，不修改 Raft membership。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T028
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `tests/view_node_discovery_test.cpp`
- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`

## Files Changed

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t028-add-observed-time-only-stale-override-rejection-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

未修改：

- ViewNode registry 生产实现
- ViewNode peer sync
- StorageNode dynamic join
- Metadata learner join
- Raft membership / quorum
- proto
- example 脚本
- `spec.md`
- `plan.md`

## What Changed

- 新增测试：
  - `ViewNodeDiscoveryTest.ObservedTimeOnlyCannotOverrideHigherSequence`
- 该测试在同一个 `incarnation_id` 下先写入 `sequence=11` 的新 `LIVE` 状态，再写入 `sequence=10` 的旧状态。
- 第二次写入故意给出更晚的 `observed_at_unix_ms=999`，并把 health 改成 `Unavailable`，模拟“旧 stale/dead 状态虽然 observed_time 更新，但不应该反向覆盖新 LIVE 状态”。
- 断言点：
  - 第二次更新返回 `kStaleIgnored`
  - `accepted_sequence` 仍保持 `11`
  - `last_seen_unix_ms` 保持首次有效更新的 `210`
  - lookup 结果仍保留 `LIVE`、`Healthy`、高 sequence 的新状态

## Boundary Checks

- 没有实现生产 merge 逻辑。
- 没有让 `observed_time` 成为跨 sequence 的唯一判断依据。
- 没有把 stale/dead 状态隐藏掉；只是验证旧状态不能覆盖新状态。
- 没有把 ViewNode 变成 Raft membership authority。
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
- 日志：`tmp/test-logs/t028-build.log`

- 测试命令：

```bash
(
  flock -n 9 || exit 99
  ctest --preset debug-tests -R "ViewNodeDiscoveryTest\\.ObservedTimeOnlyCannotOverrideHigherSequence" --output-on-failure
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：`PASS`
- 日志：`tmp/test-logs/t028-ctest.log`

说明：

- T028 当前优先覆盖“同 incarnation 内，sequence 优先于 observed_time”的 observed_time-only 覆盖路径。
- 跨 incarnation 的“observed_time 不能覆盖更高 incarnation LIVE 状态”在当前工作区已有 `ViewNodeDiscoveryTest.HigherIncarnationWinsForViewNodeObservedState` 覆盖，因此本任务没有重复再写第二个高相似度测试。

## Build Lock

- build/test 均使用了 `flock` 构建锁。
- 两次都成功获得锁，没有因为锁竞争跳过验证。

## Platform Notes

- Linux：已完成 targeted build/test，验证通过。
- Windows：pending。
- macOS：pending。

## Risks / Follow-ups

- T028 只验证了同 incarnation / 低 sequence / 更晚 observed_time 不得覆盖的边界。
- 更高 incarnation 优先于 observed_time 的路径依赖现有 `HigherIncarnationWinsForViewNodeObservedState` 和后续 T026/T030/T033 的整体语义持续保持。
- 本任务不处理 peer snapshot merge，不处理动态 join，不处理 Raft membership。

## Result

- 最终状态：`PASS`
- 已按规则把 `tasks.md` 中的 T028 checkbox 从 `[ ]` 改为 `[X]`。
- 可以进入后续任务。
