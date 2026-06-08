# T048 node app targets CMake 接入

## 1. 修改了哪些文件

- `CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t048-node-app-targets-cmake.md`

说明：

- 未修改 `apps/view_node_app.cpp`
- 未修改 `apps/metadata_node_app.cpp`
- 未修改 `apps/storage_node_app.cpp`
- 未修改 `proto/`
- 未修改测试文件

## 2. view_node_app、metadata_node_app、storage_node_app target 接入做了什么

本任务没有重构 CMake，也没有改 app 启动语义，只对现有可选 app target 接入做了最小补强：

- 将 `raft_add_optional_app_target(...)` 从“固定只链接 `raft_core`”扩展为“固定链接 `raft_core`，并允许为具体 app 追加专属私有依赖”。
- 保留了“源文件不存在时跳过 target 创建”的现有 guarded 行为，不让未落地入口破坏 configure/build。
- 把三个 node app 的正式链接关系显式写回 `CMakeLists.txt`：
  - `view_node_app`
  - `metadata_node_app`
  - `storage_node_app`
- 顺手把 `storage_client` 原本独立的补链逻辑并入同一个 helper 调用，保持 CMake 结构一致，但没有改变 `storage_client` 的 target 名称或构建语义。

本次改动的目标是把 T045/T046/T047 中已经存在的 thin startup 入口，从“占位可选 target”提升为“有明确 app-specific link boundary 的正式 target 接线”。

## 3. 三个 app target 分别链接了哪些必要依赖

三个 app target 仍统一通过 `raft_core` 获取项目主体能力，包括：

- cluster config
- node.identity
- ViewNode registry / view client / view service adapter
- Raft core / metadata control-plane
- StorageNode registry / service / chunk store
- `raft_core` 已公开导出的 `raft_proto`、`storage_node_proto`

在此基础上，本任务新增了显式 app-specific 私有依赖：

- `view_node_app`
  - `raft_core`
  - `view_proto`
- `metadata_node_app`
  - `raft_core`
  - `metadata_proto`
- `storage_node_app`
  - `raft_core`
  - `storage_node_proto`

额外说明：

- `storage_client`
  - `raft_core`
  - `view_proto`
  - `metadata_proto`
  - `storage_node_proto`

这里对 `storage_client` 只是把已有单独 `target_link_libraries(...)` 收拢进 helper 参数，不是新增业务依赖。

## 4. 是否保持已有 target、test、preset 不变

保持。

- 未删除或重命名任何已有 target。
- 未破坏 `raft_demo`、`raft_metadata_client`、`storage_client`、测试 target 或 preset。
- 未改构建 preset。
- 未改测试组织方式。
- 未改 app 名称、CLI 契约或业务行为。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `raft_core` 已经聚合了较多模块能力，因此三个 thin app 即使只链 `raft_core` 也能构建通过。T048 的价值主要是把 app-specific 依赖边界显式写回 CMake，而不是修复无法构建的问题。
- `view_node_app` 当前 thin startup 使用的是 health check + gRPC lifecycle boundary，而不是在本任务中进一步把完整 ViewNode service adapter 强行接到 app target；这符合 T045/T048 的 thin boundary，不建议在本任务越界扩展。
- configure 过程中仍会出现 `FetchContent` 的 `CMP0135` dev warning；这不是本任务引入，也不属于 T048 的修复范围。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

## 7. 验证命令和结果

### diff 检查

执行了：

```bash
git diff -- CMakeLists.txt \
  apps/view_node_app.cpp \
  apps/metadata_node_app.cpp \
  apps/storage_node_app.cpp \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t048-node-app-targets-cmake.md
```

结果：

- 本任务实际业务改动集中在 `CMakeLists.txt`
- 三个 app 源文件未改
- `tasks.md` 仅将 `T048` 标记为完成

### app targets build

执行了：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target view_node_app metadata_node_app storage_node_app'
```

结果：

- `view_node_app` 链接通过
- `metadata_node_app` 链接通过
- `storage_node_app` 链接通过

### 最小 help smoke

执行了：

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/view_node_app --help && ./build/linux/safe/metadata_node_app --help && ./build/linux/safe/storage_node_app --help'
```

结果：

- 三个 app 都成功输出各自的 `Usage` 帮助信息
- 说明 target 接线后，入口至少能独立启动到参数解析边界
