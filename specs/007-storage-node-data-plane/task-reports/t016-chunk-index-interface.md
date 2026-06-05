# T016 Chunk Index Interface

## 修改文件

- `modules/store/AGENTS.md`
- `modules/store/index/chunk_index.h`
- `modules/store/index/chunk_index.cpp`
- `modules/store/index/module-notes.md`
- `modules/store/index/AGENTS.md`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 新增 `modules/store/index` 模块，定义 `ChunkIndex` 抽象接口和 `ShardedChunkIndex` 基础内存实现。
- 支持 `Insert / Update / Find / Remove / List` 五个基础接口，并明确返回语义：
  - 重复插入：`kAlreadyExists`
  - 缺失更新/查询/删除：`kNotFound`
  - 非法参数：`kInvalidArgument`
- 新增 `ChunkIndexConfig`、`ChunkIndexListOptions` 和各类 response 结构，为 shard 数、分页大小、状态过滤、前缀过滤和 `snapshot_epoch` 预留扩展点。
- `ShardedChunkIndex` 采用按 `chunk_id` 分片的 map 结构，并在写入/更新时回填 `ChunkIndexEntry.lock_shard`，为后续 T018 的 per-chunk lock / lock striping 做准备。
- 将新模块接入 `raft_core` 构建。
- 新增 `modules/store/index/module-notes.md` 和 `modules/store/index/AGENTS.md`，并更新 `modules/store/AGENTS.md` 以纳入新子模块。
- 将 `tasks.md` 中 T016 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只新增平台无关的内存索引结构，不涉及 Windows 文件 API、路径编码或平台锁语义。
- 因此本任务不需要新增 `T016-WIN`。

## 是否通过 T016

- 通过

## 是否可以进入 T017

- 可以进入 T017

## 当前任务发现的不合理点 / 警告 / 风险

- `ShardedChunkIndex::List()` 当前已经有有界分页和 `snapshot_epoch` 占位，但并发修改下的稳定分页语义还没有收紧，已记录到 `common-risk-notes.md`。
- 本任务没有实现锁或并发控制，这是有意留给 T018 的边界，不是遗漏。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T016 标记为完成。

## 是否新增 module-notes.md / AGENTS.md

- 是。新增了 `modules/store/index/module-notes.md` 和 `modules/store/index/AGENTS.md`。
- 同时更新了 `modules/store/AGENTS.md`，把新 `index/` 子模块纳入模块索引。

## common-risk-notes.md 读取结果

- 读取到已有 3 项风险：
  - T001 `.specify` feature-dir 误指向 `006`
  - T009 checksum fixture 与生产 SHA-256 语义不一致
  - T014 Windows durable file 缺少实机验证

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T016 分页在并发修改下的稳定性风险。
- 删除 0 项。
- 解决 0 项。
- 保留 3 项既有风险：T001、T009、T014。
