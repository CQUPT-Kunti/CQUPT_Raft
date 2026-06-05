# T018 Per-Chunk Lock Striping

## 修改文件

- `modules/store/index/chunk_index.h`
- `modules/store/index/chunk_index.cpp`
- `tests/store_chunk_index_test.cpp`
- `modules/store/index/module-notes.md`
- `modules/store/index/AGENTS.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `ChunkIndex` / `ShardedChunkIndex` 新增 `AcquireChunkLock()` 和 `ChunkLockGuard`，提供 move-only 的 chunk 级 RAII guard。
- 在 `ChunkIndexConfig` 中新增 `lock_stripe_count`，实现基于 `chunk_id` 稳定 hash 的 lock striping。
- 为 `ShardedChunkIndex` 的 shard 内 `unordered_map` 补充读写锁保护，避免并发访问容器时出现数据竞争。
- 扩展 `tests/store_chunk_index_test.cpp`，覆盖：
  - 非法 chunk_id 锁请求
  - 同一 chunk 锁串行
  - 不同 chunk 在不同 stripe 时可并行
  - guard 析构后再次获取同一 chunk 锁
- 更新 `module-notes.md`，补充 shard 锁、striped lock、guard 生命周期和当前并发边界说明。
- 更新 `modules/store/index/AGENTS.md`，补充并发语义维护约束。
- 将 `tasks.md` 中 T018 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_chunk_index" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只使用标准 C++ `mutex` / `shared_mutex` / RAII lock，不引入平台专属线程或文件 API。
- 因此本任务不需要新增 `T018-WIN`。

## 是否通过 T018

- 通过

## 是否可以进入 T019

- 可以进入 T019

## 当前任务发现的不合理点 / 警告 / 风险

- `ShardedChunkIndex::List()` 现在有基础容器级并发保护，但并发修改下的稳定分页快照仍未解决。
- `AcquireChunkLock()` 解决的是 chunk 级冲突串行；未来业务流程如果不在 write/delete/repair 主入口显式持有 guard，仍可能在多步骤状态切换中发生交错。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T018 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/index/module-notes.md`
- 更新了 `modules/store/index/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 5 项风险：T001、T009、T014、T016、T018。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T018，记录后续 store 业务入口必须显式持有 chunk guard 的依赖风险。
- 删除 0 项。
- 解决 0 项。
- 保留 4 项既有风险：T001、T009、T014、T016。
- 其中 T016 风险已更新描述：T018 已补 per-chunk 串行化和容器并发保护，但并发分页快照一致性仍待后续任务处理。
