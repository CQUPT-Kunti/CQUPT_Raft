# T001 Store Module Placeholder

## 修改文件

- `modules/store/common/store_types.h`
- `modules/store/common/store_types.cpp`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t001-store-module-placeholder.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 新建 `modules/store/common/` 最小占位模块。
- 在 `store_types.h/.cpp` 中增加了不带业务实现的基础占位类型：`StorageNodeId`、`ChunkId`、`ChunkLocation`、`StoreModuleStage`。
- 将 `modules/store/common/store_types.cpp` 接入根 `CMakeLists.txt` 的 `raft_core`，验证空模块可被当前工程编译。
- 未创建 `modules/raft/storage_node/`，未新增测试、proto、KV 路径或数据面实现。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
- 结果：PASS

## 是否通过 T001

- 是

## 是否可以进入 T002

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- 已新增公共风险记录到 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`。
