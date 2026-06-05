# T026 Local Concurrency Stress

## 修改文件

- `tests/store_concurrency_stress_test.cpp`
- `tests/CMakeLists.txt`
- `node-data/.gitignore`
- `modules/store/chunk/module-notes.md`
- `modules/store/chunk/AGENTS.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/store_concurrency_stress_test.cpp`，为 `LocalDiskChunkStore` 增加 Linux-primary 的本地高并发 chunk IO 压力测试。
- 测试把 `LocalDiskChunkStore::data_dir` 固定到仓库根目录的 `node-data/t026-local-concurrency-stress/`，并在测试开始前清理旧内容，结束后保留目录供人工查看。
- 覆盖 128 个不同 chunk 的并发 `WriteChunk`，验证不同 chunk 可并行推进且写入后 `chunks/live` 落地成功、`chunks/staging` 不残留。
- 覆盖写入后并发 `ReadChunk` / `StatChunk(verify_checksum=true)` / 分页 `ListChunks`，验证 payload 与 checksum 一致。
- 覆盖同一 chunk 的并发冲突写入，验证最终只允许一个 payload 胜出；同内容幂等成功，不同内容返回 `kConflict`。
- 结合 `BoundedStorageExecutor` 覆盖 delete/read/stat/list 交错与 backpressure：显式使用固定 `worker_count=8`、`queue_capacity=16`，在过载时重试并要求出现 `overloaded`，从而验证没有无界队列增长。
- 收尾阶段用单线程分页 `ListChunks` 校验 `LIVE` / `DELETED` 数量，并把当前并发语义边界补充到 `modules/store/chunk/module-notes.md` 与 `modules/store/chunk/AGENTS.md`。
- 新增 `node-data/.gitignore`，避免 T026 运行产物被提交。

## 并发规模和覆盖场景

- 128 个不同 chunk 并发写入
- 128 个已写入 chunk 的并发读/查，外加 8 个并发分页 list probe
- 16 个同一 chunk 的并发冲突写入（8 个 payload A，8 个 payload B）
- 48 个 chunk 的并发删除，并与 128 个读、48 个 stat、8 个 list 交错
- 有界执行器并发参数：
  - `worker_count=8`
  - `queue_capacity=16`

## node-data 可视化测试数据目录路径和保留内容

- 目录路径：`node-data/t026-local-concurrency-stress/`
- 测试结束后会保留：
  - `chunks/live/` 下仍存活的 final chunk 文件
  - `chunks/staging/` 目录本身（预期为空）
  - 已删除 chunk 对应的目录层级可能保留，但 final 文件应已移除
- 当前 `ChunkIndex` 是内存结构，`DELETED` 状态和分页结果不能直接从磁盘目录反推出；可视化内容主要反映 live/staging/final 文件布局，而不是 index 全量状态。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_concurrency_stress" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "local_disk_chunk_store|store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node-concurrency`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 需要单独 Windows 实机验证。
- 原因：T026 真实覆盖了 chunk 文件的并发写、读、删和 publish 后读取路径，Windows 下仍可能暴露 sharing violation、open-handle delete、unlink 可见性和 durable publish 差异。
- 当前环境没有 Windows 编译/测试能力，不能宣称 Windows PASS。
- 已新增 `T026-WIN`，但不阻塞当前 Linux 环境下的 T026 收口。

## 是否通过 T026

- 通过

## 是否可以进入 T027

- 可以进入 T027

## 当前任务发现的不合理点 / 警告 / 风险

- `ReadChunk` 当前不持有 chunk guard；在 delete/read 交错时，当前 contract 允许返回 `kOk`、`kNotFound` 或显式 `kIoError`，测试按这条已实现语义验收，没有假定更强顺序保证。
- `ListChunks` 的并发分页快照一致性仍未解决；T026 只在单线程收尾阶段对分页结果做精确断言，因此 T016 风险仍需保留。
- `node-data/t026-local-concurrency-stress/` 会保留运行产物，便于人工检查；已通过 `node-data/.gitignore` 避免提交污染。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`：
  - 将 T026 标记为完成
  - 新增 `T026-WIN`，记录 Windows 并发实机验证待办

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 更新了 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 更新后保留/新增的风险包括：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021、T023、T024、T025、T026。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T026，记录 Windows 并发文件语义仍待实机验证。
- 删除 0 项。
- 解决 0 项。
- 保留 11 项已有风险。
- 其中 T018 风险已更新描述：`LocalDiskChunkStore::WriteChunk()` / `DeleteChunk()` 的 guard 主路径已被 T026 并发压力覆盖，但未来新增业务入口仍需显式持有 chunk guard。
