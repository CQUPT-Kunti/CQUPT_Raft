# T024 Local Disk Read Chunk

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.cpp`
- `tests/local_disk_chunk_store_test.cpp`
- `modules/store/chunk/module-notes.md`
- `modules/store/chunk/AGENTS.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 实现 `LocalDiskChunkStore::ReadChunk()` 的最小真实读取链路：
  - 必要时先 `Initialize()`
  - 校验 `request_id`
  - 拒绝当前阶段未支持的 `range` 读取
  - 先从 `ChunkIndex` 查找 chunk
  - 只允许 `LIVE` 状态进入读取路径
  - 解析 final path，并确认 final 文件存在
  - 读取整个 final 文件 payload
  - 校验读取大小与 index metadata 一致
  - 计算实际 SHA-256 checksum
  - 校验实际 checksum 与 index checksum 一致
  - 如果请求带 `expected_checksum`，继续校验请求期望
  - 仅在全部通过后返回 payload
- 扩展 `tests/local_disk_chunk_store_test.cpp`，覆盖：
  - Write 后 Read 成功返回原小型 payload
  - Write 后 Read 成功返回真实二进制 fixture
  - empty payload 写后读成功
  - `expected_checksum` mismatch 返回明确错误
  - final 文件被篡改后返回 `kCorrupted`
  - index 缺失返回 `kNotFound`
  - 非 `LIVE` 状态中的 `CORRUPTED` / `STAGING` 不可读
  - range read 当前返回明确 `kUnsupported`
  - final 文件缺失时不会回退读 staging 文件
- 更新 `module-notes.md` / `AGENTS.md`，补充 `ReadChunk` 流程、helper 语义和当前边界。

## 是否使用真实二进制 fixture；如果未使用，说明原因

- 使用了真实二进制 fixture。
- 实际使用的是仓库中存在的 `tests/test_file/test_file.deb`。
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

- 本任务的新增逻辑主要依赖 `LocalDiskChunkStore`、`std::filesystem` 和统一的 chunk layout / checksum helper，没有新增 Windows 专属分支。
- 当前环境没有 Windows 编译/测试能力，不能宣称 Windows PASS。
- 本任务不新增 `T024-WIN`；Windows 真实 publish/read 路径验证仍依赖既有 `T014-WIN` / `T023-WIN`。
- 当前判断：**Windows 待验证**。

## 是否通过 T024

- 通过

## 是否可以进入 T025

- 可以进入 T025

## 当前任务发现的不合理点 / 警告 / 风险

- `ReadChunk` 当前固定为 full read，不支持 range read。
- `ReadChunk` 检测到大小或 checksum 与 index metadata 不一致时，会返回明确错误，但不会在这一轮自动把本地 index 状态回写成 `CORRUPTED` / `QUARANTINED`。
- `LocalDiskChunkStore` 仍未实现 restart rebuild / stale staging cleanup。
- Windows 上的真实发布后读取路径仍缺实机验证。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T024 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 更新了 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 9 项既有风险：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021、T023。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T024，记录读路径发现损坏时当前只返回错误，还没有自动把 index 状态写回 `CORRUPTED` / `QUARANTINED`。
- 删除 0 项。
- 解决 0 项。
- 保留 9 项已有风险：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021、T023。
