# T025 Local Disk Delete Stat List

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.cpp`
- `tests/local_disk_chunk_store_test.cpp`
- `modules/store/chunk/module-notes.md`
- `modules/store/chunk/AGENTS.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t023-local-disk-write-chunk.md`
- `specs/007-storage-node-data-plane/task-reports/t024-local-disk-read-chunk.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 实现 `LocalDiskChunkStore::DeleteChunk()`：
  - 先校验 `request_id`
  - 通过 `ChunkIndex::AcquireChunkLock()` 串行化同 chunk 删除
  - 缺失 chunk 返回幂等成功并标记 `already_missing=true`
  - 如果请求带 `expected_checksum`，删除前先校验当前 metadata / 文件 checksum
  - 只删除 final 文件，不回退删除 staging 文件
  - 删除成功后把 index 状态更新为 `DELETED`
- 实现 `LocalDiskChunkStore::StatChunk()`：
  - 只查 `ChunkIndex`
  - 支持返回本地 metadata / state / size / checksum
  - `verify_checksum=true` 时，对 `LIVE` chunk 做 final 文件读取和 checksum 校验
- 实现 `LocalDiskChunkStore::ListChunks()`：
  - 只通过 `ChunkIndex` 列举，不扫描文件系统
  - 支持 `state_filter`
  - 支持 `page_token` / `page_size`
- 扩展 `tests/local_disk_chunk_store_test.cpp`，覆盖：
  - `StatChunk` 返回 `LIVE`
  - `ListChunks` 的 `LIVE` 过滤和分页
  - `ListChunks` 的 `DELETED` 过滤
  - `DeleteChunk` 成功后不可再读
  - repeated delete 幂等
  - `expected_checksum` mismatch 不误删
  - unknown chunk 删除幂等
  - `ListChunks` 不返回未登记 final 文件
- 修正测试和报告里的二进制 fixture 主路径为 `tests/test_file/test_file.deb`，旧 `test/test_file/test_file.deb` 仅保留为 fallback 历史路径说明。
- 更新 `module-notes.md` / `AGENTS.md`，补充删除、查询、分页和当前边界说明。
- 将 `tasks.md` 中 T025 标记为完成，并新增 `T025-WIN` 待验证任务。

## 是否使用 tests/test_file/test_file.deb 作为二进制 fixture；如果未使用，说明原因

- 使用了 `tests/test_file/test_file.deb` 作为真实二进制 fixture。

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

- 本任务真实走到了 chunk 文件删除和本地 stat/list 路径。
- 当前环境没有 Windows 编译/测试能力，不能宣称 Windows PASS。
- 已在 `tasks.md` 新增 `T025-WIN`，用于后续验证 Windows 下的 sharing violation / remove semantics 与 stat/list 路径行为。
- 当前判断：**Windows 待验证**。

## 是否通过 T025

- 通过

## 是否可以进入 T026

- 可以进入 T026

## 当前任务发现的不合理点 / 警告 / 风险

- `DeleteChunk` 当前把 `DELETED` 条目保留在内存 index 里，但这不是持久 tombstone；重启后是否还能重建，仍受 T021 恢复缺口影响。
- `StatChunk(verify_checksum=true)` 和 `ReadChunk` 一样，当前发现损坏只返回明确错误，不会自动把 index 状态回写成 `CORRUPTED` / `QUARANTINED`。
- `ListChunks` 仍沿用 `ChunkIndex` 当前的分页语义，不提供并发修改下的稳定快照。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`：
  - 将 T025 标记为完成
  - 新增 `T025-WIN`，记录 Windows 删除语义待验证任务

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 更新了 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 10 项既有风险：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021、T023、T024。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T025，记录 Windows 删除语义仍待实机验证。
- 删除 0 项。
- 解决 0 项。
- 保留 10 项已有风险：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021、T023、T024。
