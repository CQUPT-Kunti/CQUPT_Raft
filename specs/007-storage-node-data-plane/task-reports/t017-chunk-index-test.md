# T017 Chunk Index Test

## 修改文件

- `tests/store_chunk_index_test.cpp`
- `tests/CMakeLists.txt`
- `modules/store/index/module-notes.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/store_chunk_index_test.cpp`，为 `ChunkIndex / ShardedChunkIndex` 补充单元测试，覆盖：
  - insert 成功
  - duplicate insert
  - repeated update
  - missing update / find / remove
  - remove 成功
  - state filter
  - quarantine filter
  - sharded list
  - pagination / page_token 语义
- 在 `tests/CMakeLists.txt` 注册 `store_chunk_index`，接入 `storage-node` 和 `platform-neutral` 标签。
- 更新 `modules/store/index/module-notes.md`，明确 T017 当前钉住的是单线程 list / pagination 语义，并发下稳定分页仍留给后续任务。
- 维护 `common-risk-notes.md`，保留并收紧 T016 的并发分页风险描述。
- 将 `tasks.md` 中 T017 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_chunk_index" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS，包含 `store_types`、`store_durable_file`、`store_chunk_index`
- `ctest --test-dir build/linux -N -L platform-neutral`
  - PASS，`store_chunk_index` 已注册到 `platform-neutral`
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只新增平台无关的内存索引单元测试，不涉及平台文件 API、路径编码或 Windows 专属分支。
- 因此本任务不需要新增 `T017-WIN`。

## 是否通过 T017

- 通过

## 是否可以进入 T018

- 可以进入 T018

## 当前任务发现的不合理点 / 警告 / 风险

- `ShardedChunkIndex::List()` 的单线程分页语义已经被测试钉住，但并发修改下的稳定分页保证仍未解决。
- 本任务没有引入并发压力测试或 per-chunk lock，这部分仍属于 T018 的边界。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T017 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/index/module-notes.md`，补充 T017 后的分页语义边界。
- 未修改 `modules/store/index/AGENTS.md`，当前模块约束仍适用。

## common-risk-notes.md 读取结果

- 读取到 4 项风险：
  - T001 `.specify` feature-dir 误指向 `006`
  - T009 checksum fixture 与生产 SHA-256 语义不一致
  - T014 Windows durable file 缺少实机验证
  - T016 并发修改下的稳定分页风险

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 0 项。
- 删除 0 项。
- 解决 0 项。
- 保留 4 项已有风险。
- 其中 T016 风险已更新描述：T017 已补单线程分页测试，但并发稳定分页仍待 T018 或后续任务处理。
