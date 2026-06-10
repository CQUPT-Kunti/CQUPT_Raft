# T064 View-backed Placement Adapter Implementation

## 1. 修改了哪些文件

- `modules/store/placement/placement_manager.cpp`
- `modules/store/placement/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t064-view-backed-placement-adapter-impl.md`

## 2. ViewNode-backed StorageNode snapshot adapter 实现做了什么

- 在 `placement_manager.cpp` 新增了 ViewNode snapshot 路径的 eligibility 过滤与
  manager 级接线实现。
- 实现了 `PlacementManager::SelectPlacement(const PlacementRequest&, const ViewNodeBackedStorageNodeSnapshotResult&)`。
- 实现了 `PlacementManager::SelectPlacement(const PlacementRequest&, const ViewNodeBackedStorageNodeSnapshotAdapter&)`。
- 新增了 ViewNode liveness 到稳定诊断字符串的映射。
- 新增了 ViewNode snapshot -> `StorageNodePlacementCandidate` 候选保守过滤逻辑。
- 过滤后仍把真正的健康、磁盘压力、写过载、容量阈值和排序决策交给既有
  `ReplicaPolicySelector`，避免在 manager 层复制策略实现。

## 3. 如何过滤 dead / stale / unhealthy / capacity invalid 节点

- `dead` / `stale` / `suspect` / `unknown`
  - `snapshot.liveness != kLive` 时直接排除。
- `freshness invalid`
  - `last_seen_unix_ms > observed_at_unix_ms` 时直接排除，避免使用时间戳不自洽的观测。
- `facts incomplete`
  - `has_complete_facts == false` 时直接排除。
- `capacity invalid`
  - `has_valid_capacity_facts == false` 时直接排除。
  - 或者 `total_capacity_bytes == 0`
  - 或者 `used/available > total`
  - 或者 `used + available > total`
- `endpoint/node_id` 缺失
  - 视为不合格观测，直接排除。
- `unhealthy` / `disk pressure` / `write overload` / `capacity insufficient for request`
  - 不在 manager 层重复实现，而是保留给 `ReplicaPolicySelector`，并沿用现有排除原因。

## 4. 是否保持 ViewNode observation 与 placement policy 的职责边界

- 是。
- ViewNode snapshot 只作为 observation facts 输入。
- `PlacementManager` 只做保守过滤、候选组装和 decision reason 汇总。
- `ReplicaPolicySelector` 仍是最终 placement policy 执行者。
- 实现中未把 ViewNode 注册状态解释为 Raft membership，也未引入对象可见性 authority。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 T063 接口没有显式的 freshness validity 布尔位，因此本次只能基于
  `liveness` 和 `last_seen_unix_ms/observed_at_unix_ms` 的自洽性做保守过滤。
- 若后续需要区分“live 但 freshness SLA 不满足”和“时间戳字段自相矛盾”，可能还要在
  adapter 输出里补充更细粒度 freshness 诊断字段，但这不影响当前 T064 完成。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `risk-register.md`。
- `common-risk-notes.md` 在当前 feature 目录下不存在，因此未修改。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- modules/store/placement/placement_manager.cpp \
  modules/store/placement/placement_manager.h \
  modules/store/placement/module-notes.md \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t064-view-backed-placement-adapter-impl.md
```

- 结果：已执行。
- 说明：当前工作区的 diff 同时包含 `placement_manager.h` 与 `tasks.md` 中其他任务的既有差异；
  本任务未修改 `placement_manager.h`，只新增 `placement_manager.cpp` 实现、最小模块说明、
  `T064` 勾选和任务报告。

### 最小构建验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_store_placement_manager' \
  || echo "build lock busy, skip test_store_placement_manager build in this window"
```

- 结果：PASS。
- 命令实际成功完成 `debug-ninja-safe` configure，并编译通过 `test_store_placement_manager`。

### placement 相关测试

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "placement|store_placement" --output-on-failure' \
  || echo "build/test lock busy, skip placement tests in this window"
```

- 测试命令：`flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "placement|store_placement" --output-on-failure'`
- 结果：PASS。
- 通过测试：
  - `store_placement_policy`
  - `store_placement_manager`
