# T015 任务报告

## 做了什么

本任务只在 `apps/view_node_app.cpp` 内补齐 ViewNode 启动时的 identity wiring 顺序，不修改 StorageNode/MetadataNode app，不实现 ViewNode self refresh loop，也不实现 peer sync。

本次完成的内容：

- 保持 ViewNode 启动先执行统一 `LoadOrCreateNodeIdentity(...)`。
- 在 durable identity 成功 load/create 后，新增 `CreateProcessIncarnation(...)` 接入。
- 保证 self registration 发生在 validated identity 和 process incarnation 生成之后。
- self registration 与 startup register request id 改为使用 validated `node_id`，不再依赖 startup 配置副本。
- startup 日志补充 `node_type=view`、`incarnation_id`、`identity_path` 诊断信息。

## 修改了哪些文件

- `apps/view_node_app.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t015-wire-viewnode-identity-load-create-into-view-node-app-before-self-registration.md`

## ViewNode app 现在如何 load/create identity

当前启动顺序如下：

1. 解析 cluster config 和 ViewNode 启动参数。
2. 解析 `cluster_id`、ViewNode `node_id`、`data_dir`、`listen endpoint`。
3. 调用统一 `LoadOrCreateNodeIdentity(...)`：
   - `node_type = view`
   - `raft_id = nullopt`
   - `durability_mode = required`
4. 如果 `node.identity` 不存在，则首次创建 durable view identity。
5. 如果 `node.identity` 已存在，则按 `cluster_id` / `node_type` / `node_id` / `source` 校验并复用。
6. identity 校验通过后，再调用 `CreateProcessIncarnation(...)` 生成本次进程启动实例身份。
7. 只有在上述两步都成功后，才创建 registry 并进行 startup self registration。

失败边界保持 fail-fast：

- identity mismatch / corrupt / unsupported 不会被当成 first-start。
- process incarnation 创建失败不会继续启动匿名 ViewNode。
- ViewNode identity 仍然禁止携带 Metadata `raft_id`。

## self registration 是否已改为使用 validated node_id

是。

`MakeSelfRegistration(...)` 现在显式接收 `const NodeIdentity&`，并使用：

- `identity.cluster_id`
- `identity.node_id`

startup register request id 也改为基于 `identity.node_id` 生成。

这保证 self registration 使用的是已经过 durable load/create 校验后的长期身份，而不是 startup 配置副本。

## process incarnation 是否已接入

已接入。

当前 `view_node_app` 会在 `EnsureNodeIdentity(...)` 成功后调用：

```cpp
CreateProcessIncarnation(identity_state.identity)
```

并将生成出的 `incarnation_id` 输出到启动日志中。

说明：

- 本任务只把 process incarnation 接到 app 启动边界。
- 当前 registry/register payload 还没有携带 incarnation 字段；该缺口留给 Phase 3 的 self refresh / registry merge 后续任务处理。

## 本任务没有实现的内容

以下内容不在 T015 范围内，本次没有实现：

- ViewNode 周期性 self refresh loop
- ViewNode peer sync
- registry merge 的 incarnation/sequence 冲突裁决
- ViewNode 作为任何 Raft membership authority 的行为

## 验证命令

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target view_node_app test_node_identity test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\.|ViewNodeDiscoveryTest\\." --output-on-failure
```

## 验证结果

- build: PASS
- test: PASS
- `NodeIdentityTest.*`：PASS
- `ViewNodeDiscoveryTest.*`：PASS

日志文件：

- `tmp/test-logs/t015-build.log`
- `tmp/test-logs/t015-ctest.log`

## PASS / FAIL / SKIPPED

PASS

本任务未执行 local RPC `qidong.sh` / `rpc_demo.sh status` smoke。

原因：

- T015 的目标是 app 启动 wiring 边界；
- 当前 targeted build + `NodeIdentityTest` + `ViewNodeDiscoveryTest` 已覆盖本任务最直接的回归面；
- self refresh loop 与 peer sync 尚未进入本任务范围。

## Linux / Windows

- Linux：已执行 targeted build/test，验证通过。
- Windows：未实机验证，标记 pending。

## 是否可以进入 T016 / Phase 3

可以进入 T016 / Phase 3。

当前 `view_node_app` 已满足：

- self registration 之前先获得 validated durable identity；
- 每次启动生成新的 process incarnation；
- 启动日志具备 identity/incarnation 基础诊断；
- 没有把 ViewNode 扩展成 ID authority 或 Raft membership authority。

后续仍需在 Phase 3 继续补齐：

- self refresh loop
- incarnation/sequence 进入 registry/state merge
- 超过 TTL 后 ViewNode 自身不应错误变为 stale/dead
