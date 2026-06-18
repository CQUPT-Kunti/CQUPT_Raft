# T014 任务报告

## 做了什么

本任务把 Phase 2 已有的 identity lifecycle 能力接入到 `apps/storage_node_app.cpp`，让 StorageNode 启动时不再只依赖配置中的临时 `node_id`，而是：

- 先按 `cluster_id`、`role=storage`、`identity_file(data_dir/node.identity)` 走统一的 `LoadOrCreateNodeIdentity()`。
- 在 durable identity 成功 load/create 后，立即生成本次进程的 `ProcessIncarnation`。
- 后续 chunk store、本地 registry seed、ViewNode register/heartbeat、启动日志统一使用持久 `node_id` 和本次 `incarnation`。

本任务没有实现 StorageNode dynamic join、placement、ViewNode peer sync，也没有改协议语义或 Raft membership。

## 修改了哪些文件

- `apps/storage_node_app.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t014-wire-storagenode-identity-load-create-into-storage-node-app.md`

本任务没有修改：

- `modules/cluster/cluster_config.h/.cpp`
- `tests/node_identity_test.cpp`
- `tests/storage_heartbeat_registry_test.cpp`
- `tasks.md`

## StorageNode app 现在如何 load/create identity

启动流程现在是：

1. `ResolveStartupConfig()` 从现有 `cluster.json` 解析 StorageNode 本地启动配置。
2. `EnsureNodeIdentity()` 构造 storage role 的 `NodeIdentity` 期望值：
   - `cluster_id`
   - `node_type=storage`
   - `node_id`
   - `raft_id=nullopt`
3. 调用统一 `LoadOrCreateNodeIdentity()`：
   - `identity_file` 不存在时首次创建新格式 storage identity；
   - `identity_file` 已存在时加载并校验；
   - mismatch / corrupt / old-format / missing required fields 直接 fail-fast。
4. durable identity 成功后调用 `CreateProcessIncarnation()` 生成本次启动实例身份。
5. 运行态统一使用 `identity_state.identity.node_id`，不再把配置阶段的 `startup.node_id` 当作最终权威身份。

## first-start 和 restart 语义

- first-start
  - `identity_file` 缺失不是启动错误。
  - app 会创建新的 storage identity，并返回 `created_new=true`。
  - 同时生成新的 `incarnation_id` 和 `startup_sequence_base`。

- restart
  - `identity_file` 已存在时不会重新生成身份。
  - app 复用长期 `node_id`，并返回 `loaded_existing=true`。
  - 每次进程启动都会重新生成新的 `incarnation_id`。

## process incarnation 是否已接入

已接入。

接入点包括：

- `IdentityStartupState` 新增 `process_incarnation`
- 启动成功日志输出：
  - `node_type=storage`
  - `node_id`
  - `incarnation`
  - `startup_sequence_base`
  - `identity_path`
- ViewNode register request id / heartbeat request id 现在包含：
  - persistent `node_id`
  - current `incarnation_id`
- heartbeat 线程的初始 sequence 改为：
  - `identity_state.process_incarnation.startup_sequence_base`

当前边界说明：

- T014 只把 process incarnation 接入 app 内部启动与上报路径。
- 当前 ViewNode registration/heartbeat 协议本身还没有显式 `incarnation` 字段。
- 该协议层扩展仍是后续 T049 / Phase 6 缺口，不在本任务内扩大实现范围。

## 失败路径如何处理

StorageNode app 现在对以下情况保持 fail-fast：

- `identity_file` 已存在但 `cluster_id` mismatch
- `identity_file` 已存在但 `node_type` 不是 storage
- storage identity 携带有效 metadata `raft_id`
- corrupt identity file
- old-format / missing required new-format fields
- existing identity 与当前固定 `node_id` 不一致
- durable identity 成功前不会生成可用 process incarnation

失败时直接返回 identity 错误退出码，不会：

- 匿名继续启动
- 自动覆盖旧 identity
- 把 corrupt / old-format identity 当作 first-start missing identity

## 是否保持不要求完整拓扑配置

保持了。

本任务没有重构整个配置系统，也没有要求 StorageNode 持有完整拓扑 authority。当前 app 仍兼容现有 008 `cluster.json` 解析路径，只要求本地启动所需信息：

- `cluster_id`
- 当前 StorageNode 选择结果
- 本地 `data_dir`
- 本地 listen/advertise endpoint
- ViewNode seed endpoints
- capacity / failure domain / heartbeat interval 等本地配置

## 验证命令和结果

先尝试了任务文案中的 build target：

```bash
cmake --build --preset debug-ninja-low-parallel --target storage_node_app test_node_identity storage_heartbeat_registry
```

结果发现当前工程真实 target 名是 `test_storage_heartbeat_registry`，`storage_heartbeat_registry` 不存在，因此改用工程内实际 target 重新验证。

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target storage_node_app test_node_identity test_storage_heartbeat_registry
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\.|StorageHeartbeatRegistry" --output-on-failure
```

结果：

- build: PASS
- test: PASS
- `34/34` tests passed

日志：

- `tmp/test-logs/t014-build.log`
- `tmp/test-logs/t014-ctest.log`

本任务没有运行 local RPC smoke，也没有运行 roundtrip；T014 只做了 targeted build/test。

## PASS / FAIL / SKIPPED

PASS。

没有因为构建锁、target 缺失或环境限制跳过最终验证。中途只修正了任务文案与当前工程 target 名不一致的问题。

## Linux / Windows 说明

- Linux：已完成 targeted build/test 验证。
- Windows：未实机验证，标记 pending，不伪造通过。

## 是否可以进入 T015 / Phase 6

可以进入 T015。

当前 StorageNode app 已具备：

- first-start durable identity create
- restart durable identity reuse
- process incarnation wiring
- fail-fast identity validation

后续仍待后续任务处理的范围包括：

- ViewNode 协议显式承载 incarnation / sequence 语义
- StorageNode dynamic join / placement / transfer
- 更完整的 heartbeat contract 演进
