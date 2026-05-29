# T022 Local Disk Chunk Store Test

## 修改文件

- `tests/local_disk_chunk_store_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t022-local-disk-chunk-store-test.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `LocalDiskChunkStore` 基础单元测试，覆盖：
  - 有效配置初始化成功
  - `data_root`、`chunks/`、`chunks/live/`、`chunks/staging/` 目录创建
  - 临时目录隔离，不依赖固定绝对路径
  - 空 `data_dir`、空 `node_id` 的显式 `kInvalidArgument`
  - 目录冲突时的显式错误返回
  - `durable_file` 为空时按当前平台创建默认实现
  - `chunk_index` 为空时创建默认 `ShardedChunkIndex`
  - `WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 当前显式返回 `kUnsupported`
- 在 `tests/CMakeLists.txt` 注册 `local_disk_chunk_store`，接入 `storage-node` 和 `platform-neutral` 标签。
- 将 `tasks.md` 中 T022 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "local_disk_chunk_store" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS，包含 `store_types`、`store_durable_file`、`store_chunk_index`、`store_executor`、`local_disk_chunk_store`
- `ctest --test-dir build/linux -N -L platform-neutral`
  - PASS，`local_disk_chunk_store` 已注册到 `platform-neutral`
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只使用 `std::filesystem` 临时目录和平台无关初始化测试，没有引入 Windows 专属目录句柄、编码或权限逻辑。
- 因此本任务不需要新增 `T022-WIN`；Windows 实机验证风险仍由 `T014-WIN` 保留。

## 是否通过 T022

- 通过

## 是否可以进入 T023

- 可以进入 T023

## 当前任务发现的不合理点 / 警告 / 风险

- T021 保留的恢复边界仍然存在：`LocalDiskChunkStore` 现在只有初始化骨架，还没有 restart rebuild / stale staging cleanup。
- T018 的 chunk guard 风险和 T019 的 timeout/cancellation、owner-thread 边界在 T022 并未解决，后续接入真实 write/delete 时仍需遵守。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T022 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 未更新 `modules/store/chunk/module-notes.md`
- 未更新 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 8 项风险：T001、T009、T014、T016、T018、T019、T019、T021。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 0 项。
- 删除 0 项。
- 解决 0 项。
- 保留 8 项已有风险不变。
