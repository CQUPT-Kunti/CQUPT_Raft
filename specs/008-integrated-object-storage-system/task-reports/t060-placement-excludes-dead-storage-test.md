# T060 placement excludes dead StorageNode 测试

## 1. 修改了哪些文件

- `tests/store_placement_manager_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t060-placement-excludes-dead-storage-test.md`

说明：

- 本任务未修改 `tests/CMakeLists.txt`
- 本任务未修改任何生产代码
- 当前工作区中 `tests/CMakeLists.txt` 已存在与 `T060` 无关的前置未提交差异
- 当前工作区中 `tasks.md` 也已存在与 `T060` 无关的 `T058` 勾选差异；本任务只额外把 `T060` 从 `[ ]` 改为 `[X]`

## 2. T060 的 placement excludes dead StorageNode 测试做了什么

本任务在 `tests/store_placement_manager_test.cpp` 新增了一个聚焦用例：

- `StorePlacementManagerTest.ViewObservedStorageSnapshotOnlySelectsLiveHealthyFreshCapacityValidNodes`

该用例复用了当前已经可用的：

- `StorageNodeRegistrySnapshotResult`
- `PlacementManager::SelectPlacement(const PlacementRequest&, const StorageNodeRegistrySnapshotResult&)`

作为“ViewNode-observed StorageNode snapshot”的最小替身输入，避免提前实现 T063/T064 的正式 adapter。

测试构造了 5 个候选快照：

- 1 个 `live + healthy + capacity valid + low load` 节点
- 1 个 `stale` 节点
- 1 个 `dead` 节点
- 1 个 `capacity invalid` 节点
- 1 个 `live 但 ReadOnly` 节点

断言内容包括：

- placement 最终只选择 `live + healthy + capacity valid` 的节点
- `stale` 节点被以 `node registry facts are not live: Stale` 排除
- `dead` 节点被以 `node registry facts are not live: Dead` 排除
- `capacity invalid` 节点被以 `node registry capacity facts are incomplete or invalid` 排除
- `live 但 ReadOnly` 节点被以 `node health is not writable: ReadOnly` 排除
- `decision reasons` 中保留了 manager 级可诊断摘要：
  - `placement_manager evaluated 5 registry snapshot nodes`
  - `placement_manager registry snapshot kept 2 live candidates after liveness/facts filtering`

## 3. 是否覆盖 liveness / health / capacity / freshness 边界

已覆盖。

- `liveness`：
  - `Stale`
  - `Dead`
- `health`：
  - `ReadOnly`
- `capacity`：
  - `total_capacity_bytes == 0` 的无效容量事实
- `freshness`：
  - 在当前可用接口里，freshness 已被折叠为 snapshot 的 `liveness` 状态
  - 因此本测试通过 `Stale/Dead` 路径锁定“非 fresh 观测快照不得进入 placement 候选”

补充说明：

- 当前测试没有把 ViewNode 观测事实写成对象可见性依据，只把它作为 placement 输入事实。
- 当前测试没有涉及 Raft quorum、commit 或 upload/download 真实链路。

## 4. 是否发现不合理点 / 警告 / 风险

- 现阶段 `PlacementManager` 直接消费的是 `StorageNodeRegistrySnapshotResult`，而不是 T063/T064 规划中的正式 ViewNode-backed adapter。为保持任务边界，本测试显式把该 snapshot 路径当作“ViewNode-observed facts 的最小替身”使用，没有提前扩展生产接口。
- 当前工作区里存在与本任务无关的未提交差异：
  - `tests/CMakeLists.txt` 中已有 `test_view_node_discovery` 的补链改动
  - `tasks.md` 中已有 `T058` 的勾选改动
  本任务未回滚这些差异。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

## 6. 验证命令和结果

执行了 diff / 静态核对：

```bash
git diff -- tests/store_placement_manager_test.cpp \
  tests/CMakeLists.txt \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t060-placement-excludes-dead-storage-test.md
```

结果：

- 确认 `T060` 的实际新增测试位于 `tests/store_placement_manager_test.cpp`
- 未因本任务修改 `tests/CMakeLists.txt`
- `tasks.md` 已将 `T060` 标记完成

计划验证命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_store_placement_manager'
```

以及：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "ViewObservedStorageSnapshotOnlySelectsLiveHealthyFreshCapacityValidNodes" --output-on-failure'
```

结果：

- 构建锁被占用，本窗口未执行 build/test，待统一验证

说明：

- 仓库中当前实际测试 target 名称是 `test_store_placement_manager`，因此验证时使用该真实 target 名称；本任务没有为此修改 `tests/CMakeLists.txt`
