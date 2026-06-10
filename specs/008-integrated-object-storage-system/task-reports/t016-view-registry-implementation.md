# T016 ViewNode Registry 实现任务报告

## 1. 修改文件

- `modules/view/view_registry.cpp`
- `modules/view/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t016-view-registry-implementation.md`

## 2. Registry 实现内容

- 实现 `ViewNodeRegistry` pimpl、构造、析构、移动构造/赋值、`size()` 和 `config()`。
- 实现节点注册：
  - 校验 cluster_id、node_id、node_type、endpoint、observed_at 和 StorageNode capacity。
  - 同 cluster 内 endpoint 唯一性检查。
  - 同 node_id 兼容重复注册返回 idempotent。
  - node_id / endpoint / data_dir_fingerprint 不兼容时返回 conflict 和 diagnostic。
- 实现 heartbeat：
  - sequence 为 0 直接拒绝。
  - sequence 小于已接受值，或 observation 时间早于最新观测时返回 `kStaleIgnored`，不覆盖旧状态。
  - 相同 sequence 返回 idempotent，不重复覆盖。
  - 新 sequence 刷新 health、capacity、load、failure_domain、MetadataNode observation 和 leader hint。
- 实现 liveness：
  - `now_unix_ms <= last_seen` 或 elapsed <= `stale_timeout` 为 `LIVE`。
  - elapsed <= `suspect_timeout` 为 `STALE`。
  - elapsed <= `dead_timeout` 为 `SUSPECT`。
  - 超过 `dead_timeout` 为 `DEAD`。
- 实现 discovery snapshot：
  - `DiscoverMetadata` 返回 live metadata candidates、membership_epoch 最大观测值和按 observed term / observed_at 选择的 leader hint。
  - `DiscoverStorage` 支持 live_only、minimum capacity、zone、rack、require_writable 过滤。
  - `GetClusterView` 返回 ViewNode / MetadataNode / StorageNode 分类快照和 liveness warnings。
- 实现 `ToString(...)` 和 `DescribeViewRegistryDiagnostic(...)`，保证错误可测试、可诊断。

## 3. discovery-only / observation-only / non-authority 边界

- 保持 discovery-only / observation-only：实现只维护内存观测事实和候选端点。
- 未保存 object manifest 权威副本。
- 未参与 `CommitObject`。
- 未操作 StorageNode chunk 数据。
- 未修改 Raft membership、quorum、commit 规则或 election 规则。
- MetadataNode `VOTER` / leader hint 仅作为观测字段返回，不把注册结果解释为 Raft voter 变更。
- DiscoverStorage 返回 StorageNode 观测事实，不决定对象是否 `COMMITTED` 可见。

## 4. 不合理点 / 警告 / 风险

- T017 尚未完成，当前没有专门的 ViewNode registry 单元测试；本任务通过 configure/build 和头文件 include 检查验证基础编译正确性。
- `DiscoverMetadata` 在没有匹配节点时返回 `kNotFound`，并附带 diagnostic；后续 T017 可据此固定测试期望。
- 注册阶段要求 StorageNode `total_capacity_bytes > 0`，这与 data-model 的 StorageNode capacity 必须大于 0 对齐。
- 最终 diff 中观察到 `tasks.md` 的 T011、T020、T022、T023 等任务也已标记为 `[X]`；这些不是本任务修改内容。本任务只新增 T016 勾选，未验证这些任务。

## 5. common-risk-notes.md / risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 本任务风险已记录在本报告中，未发现需要扩大到风险登记文件的事项。

## 6. 验证命令和结果

```bash
git diff -- modules/view/view_registry.cpp modules/view/view_registry.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t016-view-registry-implementation.md
```

结果：PASS，目标路径 diff 显示 `modules/view/module-notes.md` 同步更新，以及 `tasks.md` 中 T016 已标记为 `[X]`。

补充说明：`modules/view/view_registry.cpp` 和本报告是新增未跟踪文件，普通 `git diff -- <path>` 不展示其正文；已通过 `git status --short -- modules/view/view_registry.cpp modules/view/view_registry.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t016-view-registry-implementation.md` 确认目标文件状态。

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

结果：PASS。

补充验证：

```bash
printf '#include "view/view_registry.h"\n' | g++ -std=c++20 -Imodules -x c++ -fsyntax-only -
g++ -std=c++20 -Imodules -fsyntax-only modules/view/view_registry.cpp
```

结果：PASS。

说明：第一次 build 验证过程中曾被中断，随后重新执行同一 build 命令并完成 PASS。T017 将补充专门的 ViewNode registry 单元测试。
