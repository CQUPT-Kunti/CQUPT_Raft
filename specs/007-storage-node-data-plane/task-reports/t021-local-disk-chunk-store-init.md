# T021 Local Disk Chunk Store Init

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.h`
- `modules/store/chunk/local_disk_chunk_store.cpp`
- `modules/store/chunk/module-notes.md`
- `modules/store/chunk/AGENTS.md`
- `modules/store/AGENTS.md`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `LocalDiskChunkStore` 最小类骨架、配置结构、初始化结果和目录路径结构。
- 实现 `Initialize()`，完成 `data_root`、`chunks/`、`chunks/live/`、`chunks/staging/` 的安全解析与目录创建。
- 目录解析复用 `ResolveDurablePathUnderRoot(...)`，避免本模块重新发明一套 root 内路径拼接规则。
- 对空 `data_dir`、空 `node_id`、目录已存在但不是目录、`std::filesystem` 创建失败等情况返回明确错误分类。
- 在未注入依赖时，默认创建平台 durable file 实现和 `ShardedChunkIndex`；`executor` 继续保留为后续异步路径扩展点。
- `WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 当前统一返回明确 `kUnsupported`，不伪装成功。
- 更新 `chunk` / `store` 模块说明文档和 AGENTS，使文档与当前代码落点一致。
- 将 `tasks.md` 中 T021 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只做 `std::filesystem` 目录初始化和平台无关骨架装配，没有新增 Windows 专属目录句柄、编码或权限逻辑。
- 当前不需要新增 `T021-WIN`；Windows durable file 的实机验证风险仍由 `T014-WIN` 保留。

## 是否通过 T021

- 通过

## 是否可以进入 T022

- 可以进入 T022

## 当前任务发现的不合理点 / 警告 / 风险

- `LocalDiskChunkStore` 现在还不是“可恢复的本地 store”，仅完成初始化骨架；restart scan、stale staging cleanup 和 index rebuild 仍待后续任务实现。
- T018 的 chunk guard 风险和 T019 的 timeout/cancellation、owner-thread 边界在 T021 仍未消除，后续真实 write/delete 接入时必须继续遵守。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T021 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 更新了 `modules/store/chunk/AGENTS.md`
- 更新了 `modules/store/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 7 项既有风险：T001、T009、T014、T016、T018、T019、T019。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T021，记录 LocalDiskChunkStore 当前只完成目录初始化，尚未具备 restart rebuild / staging cleanup 能力。
- 删除 0 项。
- 解决 0 项。
- 保留 7 项已有风险：T001、T009、T014、T016、T018、T019 timeout/cancellation 边界、T019 owner-thread 边界。
