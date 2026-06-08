# T063 View-backed Placement Adapter Interface

## 1. 修改了哪些文件

- `modules/store/placement/placement_manager.h`
- `modules/store/placement/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t063-view-backed-placement-adapter-interface.md`

## 2. ViewNode-backed StorageNode snapshot adapter 接口边界做了什么

- 在 `placement_manager.h` 新增 `ViewNodeStorageLiveness`，显式表达 placement 只消费
  ViewNode 观测到的 liveness/freshness 分类，不把它和 Raft membership 绑定。
- 新增 `ViewNodeBackedStorageNodeSnapshot`，把 placement 真正需要的 StorageNode 事实
  收敛为：
  - `StorageNodePlacementCandidate candidate`
  - `liveness`
  - `last_seen_unix_ms`
  - `observed_at_unix_ms`
  - `source_sequence`
  - `has_complete_facts`
  - `has_valid_capacity_facts`
- 新增 `ViewNodeBackedStorageNodeSnapshotDiagnostic` /
  `ViewNodeBackedStorageNodeSnapshotResult`，为 adapter 失败、节点事实缺失、容量无效、
  liveness 过滤和非 authority 边界违规提供统一诊断承载。
- 新增抽象接口 `ViewNodeBackedStorageNodeSnapshotAdapter`，只定义
  `SnapshotStorageNodes()` 边界，供 T064 实现真正的 ViewNode-backed adapter。
- 为 `PlacementManager` 新增两个仅声明的重载入口：
  - 接收已经构造好的 `ViewNodeBackedStorageNodeSnapshotResult`
  - 接收 `ViewNodeBackedStorageNodeSnapshotAdapter`
- 本任务未实现任何 adapter 逻辑，也未改动现有 placement 策略实现。

## 3. 是否保持 ViewNode observation 与 placement policy 的职责边界

- 是。
- ViewNode 侧只提供 StorageNode observation facts 和 diagnostics。
- placement 侧仍保留副本筛选、排除、排序、decision reason 生成职责。
- 报头注释已明确：
  - ViewNode 不决定对象是否 `COMMITTED` 可见
  - ViewNode 注册状态不代表 Raft voter membership
  - adapter 不授予 ViewNode metadata authority 或 StorageNode 本地状态 authority

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 `PlacementManager` 新增的 ViewNode snapshot 重载只做了头文件声明，尚未在
  `placement_manager.cpp` 落地；这是 T064 的预期工作，不属于本任务范围。
- `ViewNodeBackedStorageNodeSnapshot` 复用了
  `StorageNodePlacementCandidate` 作为策略事实载体，后续如果 ViewNode 想暴露更多仅
  诊断用字段，应继续放在 snapshot 层，而不是把 placement candidate 扩成 authority
  混合体。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `risk-register.md`。
- `common-risk-notes.md` 在当前 feature 目录下不存在，因此未修改。

## 6. 验证命令和结果

### diff 检查

```bash
git diff -- modules/store/placement/placement_manager.h \
  modules/store/placement/module-notes.md \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t063-view-backed-placement-adapter-interface.md
```

- 结果：已执行。
- 说明：`tasks.md` 的该区段在当前工作区还包含 `T058`、`T060`、`T066` 的既有差异；
  本任务只把 `T063` 从 `[ ]` 改为 `[X]`，未回滚或改写其他现存任务状态。

### 最小构建验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_store_placement_manager' \
  || echo "build lock busy, skip test_store_placement_manager build in this window"
```

- 结果：PASS。
- 说明：成功完成 `debug-ninja-safe` configure，并编译通过 `test_store_placement_manager`
  最小相关 target；未新增编译错误。
