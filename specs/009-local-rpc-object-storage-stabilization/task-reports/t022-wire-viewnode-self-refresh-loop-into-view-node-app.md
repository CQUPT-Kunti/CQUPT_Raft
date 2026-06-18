# T022 任务报告

## 做了什么

本任务把 ViewNode self refresh loop 接入到了 `apps/view_node_app.cpp` 的启动/关闭生命周期里。

在现有流程中，ViewNode app 以前只会：

1. 解析配置
2. load/create identity
3. 生成 process incarnation
4. 初始化 registry / service
5. 启动时只注册一次 self record

本次在此基础上新增了一个 app 层后台线程，让运行中的 ViewNode 周期性调用 T021 提供的 `ViewNodeRegistry::RefreshSelfNode(...)`，从而持续更新 self record 的 `last_seen` / `last_sequence`，避免它因 TTL 到期而在健康运行时被判成 `STALE` / `SUSPECT` / `DEAD`。

## 修改了哪些文件

- `apps/view_node_app.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t022-wire-viewnode-self-refresh-loop-into-view-node-app.md`

本任务没有修改：

- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`
- `tests/view_node_discovery_test.cpp`
- `tasks.md`

说明：

- `speckit-implement` 通常会要求勾选 `tasks.md`，但本任务的硬约束明确禁止修改 `tasks.md`，因此本次遵守任务边界，没有越界改任务文件。

## ViewNode app self refresh loop 如何启动和停止

启动顺序现在是：

1. 解析 `cluster.json`
2. 解析 ViewNode 启动配置
3. load/create durable `NodeIdentity`
4. 生成 `ProcessIncarnation`
5. 初始化 `ViewNodeRegistry`
6. 启动时先 `RegisterNode()` 注册一次 self record
7. 启动 gRPC server
8. 启动 `self_refresh_thread`
9. 主线程等待 stop signal
10. 收到停止信号后，`self_refresh_thread.join()`
11. 再执行 `server->Shutdown()`、`completion_queue->Shutdown()` 和其余线程 join

停止边界：

- self refresh 线程使用全局 `g_stop_requested` 作为 stop flag
- `SleepWithStop()` 采用短轮询睡眠，不会无限卡住
- 退出时先等 self refresh 线程结束，再销毁 server / queue / registry，避免线程访问已释放对象

## self refresh 如何使用 node_id / incarnation / sequence

loop 每次刷新都会构造一次 `HeartbeatNodeRequest`：

- `node_id`
  - 使用已验证的 durable identity 中的长期 `node_id`
- `sequence`
  - 从 `process_state.incarnation.startup_sequence_base` 起步
  - 每次 refresh 成功后递增
- `incarnation`
  - 当前 registry payload 还没有显式 incarnation 字段
  - 本任务通过 `request_id` 和日志携带当前 `incarnation_id`
  - 同时用该 `ProcessIncarnation` 的 `startup_sequence_base` 为本次进程实例提供 sequence 边界

当前请求格式体现为：

- `request_id = view-node-self-refresh-<node_id>-<incarnation_id>-<sequence>`

因此本任务已经在 app 层消费了 Phase 2 的 process incarnation，但没有扩大到 registry payload / merge rule 的 incarnation-aware 设计；那仍是后续 Phase 4 范围。

## 是否保持 TTL 状态机

保持了。

本任务没有改 registry 的 `DetermineLiveness()`，也没有给 ViewNode self record 加任何永久 LIVE 特权。当前语义仍是：

- 正常周期性 refresh 时：
  - `last_seen_unix_ms` 持续推进
  - self record 保持 `LIVE`
- 停止 refresh 时：
  - 仍按原有 TTL 进入 `STALE`
  - 再进入 `SUSPECT`
  - 最后进入 `DEAD`

所以：

- T018 的 “self refresh beyond TTL” 边界保持成立
- T019 的 “self refresh disabled 后 TTL 正常降级” 边界也没有被破坏

## self refresh interval 策略

本任务在 app 层新增了 `self_refresh_interval` 推导逻辑：

- 优先使用 `timeouts.heartbeat_interval`
- 如果配置缺失或为 0，回退到安全默认值 `1000ms`
- 如果 liveness timeout 缺失，app 本地采用安全默认值：
  - `stale = 5000ms`
  - `dead = 15000ms`
  - `suspect` 取 `stale/dead` 中间值
- 启动时会显式检查：
  - `self_refresh_interval < stale_timeout`

如果该关系不成立，直接按配置错误 fail-fast，而不是静默启动一个不满足契约的 loop。

## 验证命令和结果

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target view_node_app test_view_node_discovery
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

- `tmp/test-logs/t022-build.log`
- `tmp/test-logs/t022-ctest.log`

## PASS / FAIL / SKIPPED

PASS。

本次没有因为构建锁、target 缺失或环境限制跳过 build/test。

local RPC status smoke：

- SKIPPED
- 原因：T022 的核心目标是 app 层 self refresh loop wiring；当前用 targeted build + `ViewNodeDiscovery` 回归已覆盖直接风险面，没有额外运行多进程 local RPC smoke

## Linux / Windows / macOS

- Linux：已完成 targeted build/test 验证
- Windows：未实机验证，pending
- macOS：未实机验证，pending

没有伪造跨平台通过结果。

## 是否可以进入 T023

可以。

当前：

- T021 已提供 registry self refresh update path
- T022 已把 self refresh loop 接入 app 生命周期

后续 T023 可以继续补 self refresh payload 中更完整的：

- `node_id`
- `endpoint`
- `incarnation`
- `sequence`
- `observed_time`
- `health`
- `liveness`

以及与后续 registry merge / peer sync 相关的更强状态边界。 
