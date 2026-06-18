# T021 任务报告

## 做了什么

本任务在 `modules/view/view_registry.h` 和 `modules/view/view_registry.cpp` 中补了一个显式的 ViewNode self refresh registry update path：

- `ViewNodeRegistry::RefreshSelfNode(const HeartbeatNodeRequest &request)`

这个入口不引入新的特权语义，也不改 TTL 状态机；它只是把 ViewNode self refresh 明确表达成一次正常的 registry update，并故意复用现有 `HeartbeatNode()` 的 sequence / observed_at / last_seen / liveness 语义。

同时在 `tests/view_node_discovery_test.cpp` 中对 T018 做了最小同步：不再假定 self refresh 会凭空发生，而是通过测试 helper 显式调用新的 registry self refresh 入口。T019 保持“不调用 self refresh”不变。

## 修改了哪些文件

- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`
- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t021-add-viewnode-self-refresh-state-update-path.md`

本任务没有修改：

- `apps/view_node_app.cpp`
- ViewNode peer sync
- StorageNode / MetadataNode / Raft membership 相关代码
- `tasks.md`

说明：

- `speckit-implement` 通常会要求勾选 `tasks.md`，但本任务的硬约束明确禁止修改 `tasks.md`，因此本次遵守任务边界，没有越界改任务文件。

## 新增或完善的 self refresh registry update path 是什么

新增公开接口：

```cpp
HeartbeatNodeResult RefreshSelfNode(const HeartbeatNodeRequest &request);
```

实现策略：

- `RefreshSelfNode()` 直接复用 `HeartbeatNode()`。
- 这样 self refresh 与普通 heartbeat 使用完全一致的：
  - `sequence > 0` 校验
  - stale / idempotent / apply 判定
  - `observed_at_unix_ms` 更新
  - `last_seen_unix_ms` 更新
  - `last_sequence` 更新
  - `MakeSnapshot()` 里的 liveness 计算

这样做的目的就是避免把 self refresh 实现成特殊豁免路径。

## self refresh 如何保持 `LIVE`

当前语义是：

1. ViewNode self record 先通过 `RegisterNode()` 建立。
2. 后续 self refresh 周期性调用 `RefreshSelfNode()`。
3. 每次 refresh 都必须带：
   - 同一个长期 `node_id`
   - `node_type=view`
   - 更新后的 `observed_at_unix_ms`
   - 同一进程内递增的 `sequence`
4. registry 在接受该 refresh 后更新 `last_seen_unix_ms` 和 `last_sequence`。
5. 之后 `LookupNode()` / `GetClusterView()` 还是用原有 TTL 规则算 liveness，但由于 `last_seen_unix_ms` 被持续推进，所以 self record 仍保持 `LIVE`。

换句话说：

- self refresh 不是取消 TTL
- self refresh 只是持续刷新 TTL 的输入时间

## self refresh disabled 后 TTL 转换是否仍保留

保留，且已验证。

因为 `RefreshSelfNode()` 没有改 `DetermineLiveness()`，也没有写任何 “view node 永远 live” 的特判，所以：

- 继续调用 self refresh：self record 保持 `LIVE`
- 停止调用 self refresh：仍按现有 TTL 依次进入
  - `STALE`
  - `SUSPECT`
  - `DEAD`

T019 的 `ViewNodeSelfRefreshDisabledAllowsTtlTransitions` 在本任务后仍通过，没有被破坏。

## 是否涉及 incarnation / sequence

- `sequence`：有，且 self refresh 明确复用现有 sequence 排序语义。
- `incarnation`：当前 `ViewNodeRegistry` 还没有完整的 incarnation-aware merge 字段与规则，本任务没有扩大到那一层。

当前收口状态：

- T021 已把 self refresh 明确成 registry 级 update path。
- 但更高 incarnation 覆盖旧 incarnation、old incarnation stale/dead 不覆盖 new live 等 merge safety，仍是后续 T026/T030 及相关任务的范围。

## 测试同步与结果

测试侧最小同步：

- `RunningViewNodeDiscoveryService` 新增了一个测试 helper，直接调用 `registry_->RefreshSelfNode(...)`
- T018 测试现在显式触发一次 self refresh
- T019 测试仍完全不触发 refresh

这保证了：

- T018 测的是“有 self refresh update path 时能维持 LIVE”
- T019 测的是“没有 self refresh 时 TTL 正常降级”

## 验证命令和结果

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
```

结果：

- build: PASS
- test: PASS
- `13/13` tests passed

日志：

- `tmp/test-logs/t021-build.log`
- `tmp/test-logs/t021-ctest.log`

## PASS / FAIL / SKIPPED

PASS。

本次没有因为构建锁、target 缺失或环境限制跳过验证。

## 是否可以进入 T022

可以。

T021 已经把 registry 内的 self refresh state update path 补齐。下一步 T022 只需要在 `apps/view_node_app.cpp` 中负责：

- 启动 self refresh loop
- 按生命周期停止 self refresh loop
- 使用当前 `node_id`、sequence、时间源去周期性调用这条 registry update path

而不需要重新设计 registry 语义。 
