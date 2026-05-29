# T023 Local Disk Write Chunk

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.cpp`
- `tests/local_disk_chunk_store_test.cpp`
- `modules/store/chunk/module-notes.md`
- `modules/store/chunk/AGENTS.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 实现 `LocalDiskChunkStore::WriteChunk()` 的最小真实写入链路：
  - 必要时先 `Initialize()`
  - 校验 `request_id`、`ChunkIdentity`、`expected_size`
  - 计算或校验 SHA-256 checksum
  - 通过 `ChunkIndex::AcquireChunkLock()` 串行化同 chunk 写入
  - 处理重复写幂等和冲突写
  - 基于 `BuildChunkPathLayout()` 生成 staging/final 路径
  - 通过 `DurableFile` 执行 staging write、flush、close、publish、directory sync
  - publish 和 directory sync 成功后才插入 LIVE index
- 扩展 `tests/local_disk_chunk_store_test.cpp`，覆盖：
  - `expected_size` mismatch
  - `expected_checksum` mismatch
  - flush 失败不更新 LIVE index
  - directory sync 失败不更新 LIVE index
  - 小型 payload 成功写入
  - 二进制 fixture 成功写入
  - same chunk 同内容重复写幂等成功
  - same chunk 不同内容重复写返回冲突
- 调整测试 fake durable writer 的默认成功语义，使其默认满足 flush/publish/sync durable boundary，避免测试夹具比真实 contract 更弱。
- 更新 `module-notes.md` / `AGENTS.md`，把 `WriteChunk` 已实现状态、helper 语义和当前未实现边界写清楚。
- 将 `tasks.md` 中 T023 标记为完成，并新增 `T023-WIN` 待验证任务。

## 是否使用 tests/test_file/test_file.deb 作为二进制 fixture；如果未使用，说明原因

- 已使用仓库内实际存在的 `tests/test_file/test_file.deb` 作为真实二进制 fixture。
- `test/test_file/test_file.deb` 在当前仓库中不存在，只作为 fallback 历史路径记录。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "local_disk_chunk_store" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务真实走到了 `DurableFile` 写入、publish 和 directory sync 边界。
- 当前环境没有 Windows 编译/测试能力，不能宣称 Windows PASS。
- 已在 `tasks.md` 新增 `T023-WIN`，用于后续验证 `WindowsDurableFile` 下的 staging/publish/directory-sync contract。
- 当前判断：**Windows 待验证**。

## 是否通过 T023

- 通过

## 是否可以进入 T024

- 可以进入 T024

## 当前任务发现的不合理点 / 警告 / 风险

- `LocalDiskChunkStore` 现在只补齐了在线 `WriteChunk` durable publish 顺序，还没有 restart rebuild、stale staging cleanup、published-but-not-indexed 重建和 quarantine/recovery。
- `WriteChunk` 已经显式持有 chunk guard，但这条约束仍需要后续 `ReadChunk` / `DeleteChunk` / repair / rebalance 入口继续遵守。
- Windows 分支的真实集成行为仍未实机验证，尤其是 `SyncDirectory()` 当前 explicit unsupported 对 `WriteChunk` 成功语义的影响。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`：
  - 将 T023 标记为完成
  - 新增 `T023-WIN`，记录 Windows 实机验证待办

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 更新了 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 8 项既有风险：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T023，记录 `WriteChunk` 接入后 Windows durable publish / directory sync 仍待实机验证。
- 删除 0 项。
- 解决 0 项。
- 保留 8 项已有风险：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021。
