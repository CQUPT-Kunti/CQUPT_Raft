# T010 ChunkStore Interface

## 修改文件

- `modules/store/chunk/chunk_store.h`
- `modules/store/chunk/chunk_store.cpp`
- `modules/store/chunk/README.md`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 新增 `modules/store/chunk/` 子模块，按当前 `modules/store/` 约定定义 `storedemo::ChunkStore` 抽象接口。
- 新增 `WriteChunk`、`ReadChunk`、`DeleteChunk`、`StatChunk`、`ListChunks` 请求/响应结构，复用已有 `ChunkIdentity`、`ChunkMetadata`、`ChunkChecksum`、`ChunkState`、`StorageNodeStatusCode`。
- 在 `raft_core` 中接入 `modules/store/chunk/chunk_store.cpp`，保证接口随主库一起编译。
- 新增 `modules/store/chunk/README.md`，用简短模块说明替代大量代码注释。
- 修正 `tasks.md` 中 T010 仍指向 `modules/store/storage_node/` 的旧路径，并标记 T010 已完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_types" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## 是否通过 T010

- 通过

## 是否可以进入 T011

- 可以进入 T011

## 当前任务发现的不合理点 / 警告 / 风险

- `tasks.md` 中 T011 及后续多处路径仍使用 `modules/store/storage_node/...`，与当前 `modules/store/<submodule>/` 约定不一致，已记录到 `common-risk-notes.md`。
- `tests/support/store_test_utils.h` 的 checksum fixture 仍与当前生产 SHA-256 语义不一致，本任务未处理，保留为后续风险。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，因为 T010 的文件路径仍写成旧的 `modules/store/storage_node/...`，与当前用户约定和实际代码落点冲突，属于文档内容纠偏。

## 是否新增/更新模块注释文档

- 是。新增 `modules/store/chunk/README.md`，简要说明模块职责、主要结构、接口函数、特殊字段和当前未实现边界。

## common-risk-notes.md 新增/删除/解决了哪些项

- 新增 1 项：后续 T011 及以后任务仍残留旧的 `modules/store/storage_node/...` 路径风险。
- 未删除已有风险项。
