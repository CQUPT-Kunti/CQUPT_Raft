# T031 Storage Node Service WriteChunk

## 修改文件

- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/module-notes.md`
- `modules/store/node/AGENTS.md`
- `modules/store/AGENTS.md`
- `tests/storage_node_service_test.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `modules/store/node/StorageNodeService`，使用真实 gRPC `CallbackService` 形态实现生产侧 `WriteChunk` adapter
- 通过构造注入 `ChunkStore`，将 proto `WriteChunkRequest` 转成 `ChunkStore::WriteChunk` 请求，并把 `ChunkStore` 结果映射回 `WriteChunkResponse`
- 新增 `tests/storage_node_service_test.cpp`，通过最小 in-process gRPC server + 真实 generated stub 直接调用 service，不启动真实进程级 RPC server
- 补充 `modules/store/node` 的模块说明和开发边界，并在 `modules/store/AGENTS.md` 增加 `node/` 子模块索引
- 将 `tasks.md` 的 T031 标记完成，并使用实际 `modules/store/node/...` 路径收口

## StorageNodeService::WriteChunk 字段映射和状态映射

- request -> store：
  - `request_id`
  - `chunk_id`
  - `object_id`
  - `version`
  - `chunk_index`
  - `offset`
  - `expected_size`
  - `expected_checksum`
  - `payload`
- 当前 `timeout_ms` / `best_effort_cancel` 只作为 contract 字段接收，不伪装成已经具备运行中取消传播
- `durability` 当前接受 `UNSPECIFIED` / `PUBLISH`，对应当前 `LocalDiskChunkStore::WriteChunk` 的 durable publish 成功语义
- store status -> proto `summary.code`：
  - `kOk` -> `OK`
  - `kAlreadyExists` -> `ALREADY_EXISTS`
  - `kNotFound` -> `NOT_FOUND`
  - `kConflict` -> `CONFLICT`
  - `kChecksumMismatch` -> `CHECKSUM_MISMATCH`
  - `kCorrupted` -> `CORRUPTED`
  - `kDiskFull` -> `DISK_FULL`
  - `kPermissionDenied` -> `PERMISSION_DENIED`
  - `kIoError` -> `IO_ERROR`
  - `kTimeout` -> `TIMEOUT`
  - `kCancelled` -> `CANCELLED`
  - `kOverloaded` -> `OVERLOADED`
  - `kNodeUnavailable` -> `NODE_UNAVAILABLE`
  - `kUnsupported` -> `UNSUPPORTED`
  - `kInvalidArgument` -> `INVALID_ARGUMENT`
- `summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 都直接反映 `ChunkStore` 返回事实

## 是否调用 ChunkStore；是否调用 RaftNode::ProposeMetadata()

- 调用 `ChunkStore::WriteChunk()`
- 不调用 `RaftNode::ProposeMetadata()`
- 不调用 metadata service / `MetadataStateMachine`

## 是否使用 tests/test_file/test_file.deb

- 是

## node-data 可视化目录路径和保留内容

- 本任务未使用 `node-data/t031-storage-node-service/`
- 测试使用 `tests/support/store_test_utils.h` 的 `ScopedStoreTestDir` 临时目录，测试结束后自动清理

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_service|write_chunk_contract" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "storage_upload|local_disk_chunk_store|store_concurrency_stress" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS

## Windows 验证判断

- 本任务只新增 gRPC service adapter 和真实 service 测试，没有新增 Windows 专属文件逻辑
- 因此本次不新增 `T031-WIN`
- 真实 Windows 数据面行为仍由既有 `T014-WIN`、`T023-WIN`、`T025-WIN`、`T026-WIN` 覆盖

## 是否通过 T031

- 是

## 是否可以进入 T032

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，本任务继续按用户指定的 `specs/007-storage-node-data-plane` 执行
- `ChunkStore::WriteChunk` 当前没有 `StorageTaskContext` 入参，所以 T031 只能接收 `timeout_ms` / `best_effort_cancel` 字段，不能伪装成已经具备运行中取消传播
- `StorageNodeClient` 仍未实现，跨 RPC deadline/cancelled/error mapping 还需 T032 收口
- upload coordinator / metadata commit gate 仍未实现，T027 暴露的 orphan chunk 风险继续保留

## 是否修正了高频文档，为什么

- 是，更新了 `tasks.md`
- 原因：将 T031 标记完成，并把实际落点固定为 `modules/store/node/...`

## 是否更新 module-notes.md / AGENTS.md / contract 文档

- 更新了 `modules/store/node/module-notes.md`
- 更新了 `modules/store/node/AGENTS.md`
- 更新了 `modules/store/AGENTS.md`
- 未更新 contract 文档

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T028 风险仍需继续维护

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 收窄：T028 不再声明“缺真实 StorageNodeService”，改为保留 `StorageNodeClient` 与跨 RPC deadline/cancelled 映射风险
