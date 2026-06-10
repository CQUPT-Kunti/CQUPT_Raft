# T059 任务报告

## 1. 修改了哪些文件

- `tests/view_node_discovery_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t059-view-node-heartbeat-liveness-tests.md`

说明：`test_view_node_discovery` target 已在前置任务中安全接入，本任务未修改 `tests/CMakeLists.txt`。
补充：当前工作树里的 `tasks.md` 还包含其他任务的既有未提交状态变化；本任务只新增了 T059 的勾选，没有回退或改写那些状态。

## 2. T059 的 heartbeat timeout / liveness transition tests 做了什么

- 在 `tests/view_node_discovery_test.cpp` 中新增 `IntegrationHeartbeatRefreshesStateAndRejectsStaleUpdates`
  - 通过 `ViewNodeClient::HeartbeatNode` 刷新 StorageNode 的健康、容量、负载和 `observed_at`
  - 断言新的 heartbeat 被应用后，`DiscoverStorage` 返回的是刷新后的状态
  - 同时发送旧 `sequence` 和旧 `observed_at` 的 heartbeat，断言被 `stale_ignored`
- 新增 `IntegrationLivenessTransitionsAppearInDiscoveryAndClusterView`
  - 使用 `RunningViewNodeDiscoveryService` 的可控时间源，不依赖真实 sleep
  - 通过推进 `now_unix_ms`，验证同一 StorageNode 在 cluster view 中依次呈现 `LIVE -> STALE -> SUSPECT -> DEAD`
  - 验证 `DiscoverStorage(live_only=true)` 在节点 stale 后不再把它当成 live 候选
  - 验证 `GetClusterView(include_dead_nodes=false)` 在节点 dead 后会把它从返回列表中排除，并带上 `liveness_excluded` 诊断

## 3. 是否验证旧 heartbeat 不覆盖新状态

已验证。

- 旧 `sequence` heartbeat 被断言为 `ViewRegistryStatusCode::kStaleIgnored`
- 新 `sequence` 但更旧 `observed_at` 的 heartbeat 也被断言为 `ViewRegistryStatusCode::kStaleIgnored`
- 后续 `DiscoverStorage` 结果继续保持最近一次有效 heartbeat 写入的健康、容量、负载、`last_sequence` 和 `last_seen_unix_ms`

## 4. 是否保持 ViewNode non-authority 边界

保持。

- 测试只验证 heartbeat、timeout 和 liveness 的观测边界。
- 没有把 heartbeat 状态用于 Raft membership 或 quorum 计算。
- 没有修改 commit、election、snapshot、recovery 语义。
- 没有涉及 object manifest、StorageNode payload 或 placement 决策权威。

## 5. 是否发现不合理点 / 警告 / 风险

- `tests/view_node_discovery_test.cpp` 现在同时承载 registry 单测、discovery 集成测试和 heartbeat/liveness 集成测试；后续如果 US3 再继续扩展，可以考虑按“registry-only”和“service/client integration”拆分。
- 当前验证命令里的 `debug-tests` preset 仍指向 `build/linux`，而按要求构建的是 `build/linux/safe`；因此实际执行测试时需要在 `build/linux/safe` 中直接调用 `ctest` 才能命中本次构建产物。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- tests/view_node_discovery_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t059-view-node-heartbeat-liveness-tests.md
```

结果：已执行。

- 本任务实际改动集中在 `tests/view_node_discovery_test.cpp`、`tasks.md` 和本报告文件
- `tests/CMakeLists.txt` 在 diff 中仅作为观察范围，本任务未修改
- `tasks.md` 的完整 diff 中还能看到当前工作树已有的其他任务状态变化，本任务只新增了 T059 的勾选

### 最小构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_view_node_discovery' \
  || echo "build lock busy, skip view_node_discovery_test build in this window"
```

结果：PASS

### 只运行 T059 heartbeat/liveness 测试

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "ViewNodeDiscoveryTest\\.Integration(Heartbeat|Liveness).*" --output-on-failure' \
  || echo "build/test lock busy, skip T059 test in this window"
```

结果：该命令未直接采用。

说明：

- `debug-tests` preset 指向 `build/linux`
- 本任务按要求使用 `debug-ninja-safe` 构建，产物位于 `build/linux/safe`
- 为避免再写入另一套 build 目录，本任务在同一把锁下直接于 `build/linux/safe` 执行等价最小命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cd build/linux/safe && ctest -R "ViewNodeDiscoveryTest\\.Integration(Heartbeat|Liveness).*" --output-on-failure' \
  || echo "build/test lock busy, skip T059 test in this window"
```

结果：PASS

- `ViewNodeDiscoveryTest.IntegrationHeartbeatRefreshesStateAndRejectsStaleUpdates`
- `ViewNodeDiscoveryTest.IntegrationLivenessTransitionsAppearInDiscoveryAndClusterView`
