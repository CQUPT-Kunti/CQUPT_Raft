# T027 Upload Close Loop

## 修改文件

- `tests/storage_upload_integration_test.cpp`
- `tests/CMakeLists.txt`
- `modules/store/chunk/module-notes.md`
- `modules/store/chunk/AGENTS.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_upload_integration_test.cpp`，建立 `MetadataStateMachine + LocalDiskChunkStore` 的最小上传闭环集成测试骨架。
- 真实表达 `CreateObject -> WriteChunk -> CommitObject` 的当前等价流程，不新增 StorageNode RPC、不新增 proto、不改 metadata/control-plane 生产语义。
- 用测试把 upload gating 的当前边界钉住：
  - pending 对象在 commit 前对 `HeadObject` / `ListObjects` 不可见
  - chunk durable 后再 `CommitObject`，对象才变为 committed 可见
  - chunk 写失败时，不进入 commit，对象继续保持不可见
  - metadata commit 失败时，对象仍不可见，但本地 durable chunk 仍保留，明确暴露当前 orphan chunk 风险

## 上传闭环覆盖场景

- `CreateObject` 创建 pending object 后，`HeadObject` / `ListObjects` 不可见
- 真实 `LocalDiskChunkStore::WriteChunk()` durable 成功后，commit 前对象仍不可见
- `CommitObject` 写入单 chunk manifest 后，对象对 `HeadObject` / `ListObjects` 可见，metadata 只记录 `chunk_id` / `size` / `checksum` / `replica_nodes`
- `WriteChunk` checksum mismatch 失败时，不提交 metadata，store 中也不留下 live chunk
- metadata commit 失败时，store 中仍保留 durable chunk，metadata 侧保持 pending/invisible

## 是否使用 tests/test_file/test_file.deb

- 使用了 `tests/test_file/test_file.deb` 作为真实二进制 payload。

## node-data 可视化目录路径和保留内容

- 目录路径：`node-data/t027-upload-close-loop/`
- 测试开始前会清理旧内容，结束后保留目录。
- 当前测试结束后可保留：
  - `chunks/live/` 下的 final chunk 文件
  - `chunks/staging/` 目录及其分片层级目录；当前预期不保留 staging 文件
- 由于 3 个 case 共用同一路径并在各自开始前清理，最终保留的是最后一个 case 的数据面产物；当前顺序下，最后保留的是“metadata commit 失败但 durable chunk 仍在”的 orphan candidate 观察目录。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "upload|local_disk_chunk_store|store_concurrency_stress" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "local_disk_chunk_store|store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务未新增新的 Windows 专属数据面实现，也没有扩展 `LocalDiskChunkStore` 的文件语义；它只是把既有 metadata 状态机与已存在的本地 chunk write 路径串成测试骨架。
- 因此本任务不新增 `T027-WIN`。
- 当前 Windows 风险仍由既有 `T014-WIN`、`T023-WIN`、`T025-WIN`、`T026-WIN` 覆盖。

## 是否通过 T027

- 通过

## 是否可以进入 T028

- 可以进入 T028

## 当前任务发现的不合理点 / 警告 / 风险

- 当前只是测试骨架，不是生产 upload coordinator。metadata 层本身仍不知道 chunk durable 是否成功；真正的 commit gate 还需要 T029-T035 的后续任务收口。
- metadata commit 失败后，当前 durable chunk 会留在 `LocalDiskChunkStore`，这是明确的 orphan chunk 风险；本任务只把它测试暴露出来，没有提前实现 abort/GC。
- `LocalDiskChunkStore` 的 Windows durable publish / delete / concurrency 实机验证仍待既有 Windows 任务处理。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T027 标记为完成。
- 没有新增 `T027-WIN`，因为本任务没有引入新的 Windows 专属实现路径，Windows 风险已由既有待验证任务覆盖。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 更新了 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取后保留/新增的风险包括：T001、T009、T014、T016、T018、T019(timeout/cancellation)、T019(owner-thread shutdown)、T021、T023、T024、T025、T026、T027。

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 1 项：T027，记录 metadata commit 失败后的 orphan chunk 风险和缺少真实 upload coordinator 的当前边界。
- 删除 0 项。
- 解决 0 项。
- 保留 12 项已有风险。
- 其中 T014/T023/T025/T026 的 Windows 待验证风险继续保留，T016/T018/T019/T021/T024 也与当前任务仍相关。
