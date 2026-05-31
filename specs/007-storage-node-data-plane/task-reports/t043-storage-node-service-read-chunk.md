# T043 Storage Node Service ReadChunk

## 修改文件

- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/module-notes.md`
- `tests/storage_node_service_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t043-storage-node-service-read-chunk.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 实现 `StorageNodeService::ReadChunk` gRPC 适配层。
- 新增 `ReadChunk` request 校验与字段转换 helper，把 proto request 转成 `ChunkStore::ReadChunk` 请求。
- 新增 `ReadChunk` response 映射 helper，把 `ChunkStore::ReadChunk` 的状态、checksum、state、payload、offset、complete/full_read 映射回 proto response。
- 保持 service 只调用注入的 `ChunkStore::ReadChunk()`，不调用 metadata / Raft，不决定 object committed 可见性。
- 扩展 `tests/storage_node_service_test.cpp`，覆盖 service 级 `ReadChunk` 成功、checksum mismatch、missing、non-live、range boundary 和 metadata 可见性边界。
- 更新 `modules/store/node/module-notes.md`，补充 `storage_node_service.cpp` 中新增 helper 和 `StorageNodeService::ReadChunk` 流程说明。

## StorageNodeService::ReadChunk 字段映射和状态映射

- request -> `ChunkStore::ReadChunk`
  - `request_id` -> `request_id`
  - `chunk_id` -> `chunk_id`
  - 当 `chunk_id` 为空时，使用 `object_id + version + chunk_index` 派生 `chunk_id`
  - `length > 0` 时，把 `offset + length` 转成 `ChunkReadRange`
  - `expected_checksum` -> `expected_checksum`
  - `verify_checksum` -> `verify_checksum`
  - `timeout_ms` / `best_effort_cancel` 当前只接收 contract，不做真实运行中取消传播
- `ChunkStore::ReadChunk` -> proto response
  - `status` / `error_detail` / `retry_after_ms` -> `summary.code` / `summary.message` / `summary.retry_after_ms`
  - `request.request_id` -> `summary.request_id`
  - `metadata.node_id` 或 service 构造的 `node_id` -> `summary.node_id`
  - `metadata.identity.chunk_id` / request / 派生 chunk id -> `summary.chunk_id` 和顶层 `chunk_id`
  - `payload` -> `payload`
  - `metadata.size`（为空时回退 `payload.size()`）-> `size`
  - `actual_checksum` 优先，否则 `metadata.checksum` -> `checksum`
  - `metadata.state` -> `state`
  - `metadata.identity.offset` -> `offset`
  - 成功且无 range request -> `complete=true`、`full_read=true`
  - 成功但带 range request -> `complete=true`、`full_read=false`
  - 失败 -> `complete=false`、`full_read=false`

## checksum mismatch / corrupted 当前边界

- `StorageNodeService::ReadChunk` 当前会把底层 `ChunkStore::ReadChunk` 返回的 `checksum_mismatch` / `corrupted` / `conflict` / `not_found` 明确映射到 proto response。
- 当前 `LocalDiskChunkStore::ReadChunk()` 在 checksum mismatch / corrupted 场景只返回明确错误，不会自动回写 `CORRUPTED` / `QUARANTINED` 状态。
- T043 没有在 service 层强行增加不合理的状态回写接口；该边界已在报告和 `common-risk-notes.md` 中保留。

## 是否调用 ChunkStore；是否调用 metadata / Raft

- 调用 `ChunkStore`：是，只调用注入的 `ChunkStore::ReadChunk()`
- 调用 metadata：否
- 调用 Raft：否

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_service|storage_read_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t043-storage-node-read-service.log`
  - PASS
  - 日志路径：`tmp/007/t043-storage-node-read-service.log`

## Windows 验证判断

- 本任务只新增 service adapter 和平台无关 service 测试，没有新增 Windows 专属文件行为。
- 当前没有 Windows 编译/测试环境，不伪造 Windows PASS。
- 本任务未新增 `T043-WIN`。

## 是否通过 T043

- 是

## 是否可以进入 T044

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- proto response 当前没有独立 `retryable` 布尔字段，T043 只能通过现有 `summary.code` / `summary.retry_after_ms` 表达状态边界，不能伪造不存在的 proto 语义。
- `StorageNodeService::ReadChunk` 已完成，但 `StorageNodeClient::ReadChunk`、read fallback 和 read replica selection 仍未实现。
- `LocalDiskChunkStore::ReadChunk()` 当前 checksum mismatch / corrupted 只返回明确错误，不自动回写 `CORRUPTED` / `QUARANTINED`；T043 保持这个边界。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。
- T019/T020 timeout/cancellation、Windows 待验证、restart rebuild / staging cleanup 风险仍存在。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/node/module-notes.md`
- 未更新 `modules/store/node/AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补充：
  - `ResolveResponseChunkId(const raft::ReadChunkRequest&, const ReadChunkResponse&)`
  - `FillSummary(const raft::ReadChunkRequest&, const ReadChunkResponse&, ...)`
  - `MakeReadValidationError(...)`
  - `TranslateReadRequest(...)`
  - `FillReadResponse(...)`
  - `StorageNodeService::ReadChunk(...)`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T043 标记完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：删除已过时的“service 尚未实现”描述，并保留 T043 后仍存在的 client/read replica selection 与 corruption 状态回写边界
- 修改了 `modules/store/node/module-notes.md`
  - 原因：补充 `ReadChunk` service 适配层的关键 helper、输入输出和边界

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：T043
- 删除：T042
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：将 ReadChunk 剩余风险从 “proto 已完成但 service/client/select 未实现” 收紧为 “service 已完成，client/select 和 corruption 状态自动回写仍待后续任务”
