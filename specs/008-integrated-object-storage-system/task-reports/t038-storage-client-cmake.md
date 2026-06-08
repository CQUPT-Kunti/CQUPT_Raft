# T038 任务报告：storage_client CMake 接入

## 1. 修改了哪些文件

- `CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t038-storage-client-cmake.md`

## 2. storage_client target 接入做了什么

- 保持现有 `raft_add_optional_app_target(storage_client apps/storage_client.cpp)` 机制不变。
- 在 `storage_client` target 已创建时，最小补充了该 target 的额外链接依赖。
- 没有重命名 target，没有改 app 入口名称，也没有调整 preset。
- 没有修改业务逻辑，没有扩展 CLI 命令。

## 3. storage_client 链接了哪些必要依赖

本任务为 `storage_client` 显式补充了：

- `view_proto`
- `metadata_proto`
- `storage_node_proto`

原因：

- T037 新增的 `apps/storage_client.cpp` 直接依赖 `ViewNodeClient`、`MetadataTransferClient`、`StorageTransferClient` 和 `ObjectTransfer`。
- 之前 `storage_client` 只通过 `raft_core` 间接链接，导致链接阶段缺少 `view::*` protobuf / gRPC 符号。
- `view_proto` 是这次修复的关键依赖；`metadata_proto` 和 `storage_node_proto` 一并显式保留，有助于让 `storage_client` target 的使用需求更清晰、独立。

## 4. 是否保持已有 target、preset 不变

- 是。
- 保持了已有 target 名称不变：
  - `raft_core`
  - `raft_demo`
  - `raft_metadata_client`
  - `storage_client`
- 保持了 `raft_add_optional_app_target(...)` 的整体风格不变。
- 没有改 `debug-ninja-safe` / 其他 preset。
- 没有新增其他 app target。

## 5. 是否发现不合理点 / 警告 / 风险

- 发现一个前序 wiring 缺口：`raft_core` 中已经包含 `modules/view/view_client.cpp` 等 008 源文件，但 `storage_client` 之前没有显式拿到 `view_proto`，所以 T037 时会在 link 阶段报 `view::*` unresolved symbols。
- 当前修复只针对 `storage_client` 做最小补丁，符合 T038“只接 storage_client target”的边界；如果后续其他 app target 也直接或间接依赖 ViewNode client，同类依赖关系仍需在各自任务中检查。
- 本任务没有修改 `apps/storage_client.cpp`；T037 中 upload 仍会把 `committed=false` 当失败，这是现阶段刻意保守的正确行为，不属于 CMake 问题。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- CMakeLists.txt apps/storage_client.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t038-storage-client-cmake.md
```

- 结果：本任务实际代码改动集中在 `CMakeLists.txt`；`apps/storage_client.cpp` 未因 T038 再做改动，符合“只做构建接入”的要求。

### storage_client 最小构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_client' || echo "build lock busy, skip storage_client build in this window"
```

- 结果：`PASS`
- 说明：
  - `cmake configure` 成功
  - `view_proto` 成功生成并归档
  - `storage_client` 成功链接为独立可编译 target

## 结论

- T038 已完成。
- `storage_client` target 现在可以被单独编译。
- 可以进入 T039 继续接入 `integrated_object_storage_e2e` 测试 target / label。 
