# T035 Add Dual ViewNode Registry Sync Test

## 做了什么

在 `tests/view_node_discovery_test.cpp` 增加了 test-level peer snapshot replay helper，并新增双 ViewNode registry sync 测试，验证一个 ViewNode 已观测到的节点状态可以通过测试层同步动作传播到另一个 ViewNode 的 registry，再被对端 discovery 查到。

本任务只补测试，没有实现生产 peer sync 网络逻辑。

## 修改文件

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 新增测试

- `ViewNodeDiscoveryTest.DualViewRegistrySyncPropagatesObservedStateToPeer`

## 测试如何验证 dual ViewNode registry sync

测试流程：

1. 构造两个独立 `ViewNodeRegistry`，分别代表 ViewNode A 和 ViewNode B。
2. 在 ViewNode A 注册一个 StorageNode，并通过 heartbeat 写入 `incarnation_id`、`sequence`、`observed_at_unix_ms`、`health`、`endpoint` 等 observed state。
3. 从 ViewNode A 的 `GetClusterView()` 读取最新 storage snapshot。
4. 用测试 helper 将该 snapshot replay 到 ViewNode B：
   - 先 `RegisterNode(...)`
   - 再按 snapshot 的 observed-state 发 `HeartbeatNode(...)`
5. 在 ViewNode B 执行 `DiscoverStorage(...)`，断言能看到来自 ViewNode A 的节点状态。
6. 再把较旧的 sequence=7 snapshot replay 到 ViewNode B，断言不会覆盖已同步的较新 sequence=8 状态，保持既有 incarnation/sequence merge ordering。

这证明了：

- dual ViewNode 间可以在测试层模拟 observed-state eventual sync；
- 同步后的状态能在另一侧 discovery 路径被发现；
- peer sync 传播不改变 Raft membership authority；
- 旧状态不会覆盖新状态。

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery > tmp/test-logs/t035-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure > tmp/test-logs/t035-ctest.log 2>&1
```

## 结果

- PASS
- `test_view_node_discovery` 构建通过
- `ViewNodeDiscovery` 相关 23/23 测试通过
- 已在 `tasks.md` 只勾选 T035

## 说明

- 本任务使用的是 test-level snapshot replay，不是生产 peer sync API。
- 生产级 peer sync contract / adapter / loop 仍留给后续 T039-T041。
