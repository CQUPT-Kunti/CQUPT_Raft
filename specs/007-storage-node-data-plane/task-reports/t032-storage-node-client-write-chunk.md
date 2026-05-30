# T032 Storage Node Client WriteChunk

## 修改文件

- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/module-notes.md`
- `modules/store/node/AGENTS.md`
- `tests/storage_node_client_test.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增生产侧 `StorageNodeClient`，实现同步 `WriteChunk` RPC 调用
- 将本地 `storedemo::WriteChunkRequest` 转成 `storage_node.proto::WriteChunkRequest`
- 将 proto `WriteChunkResponse` 和 gRPC status 映射回本地 `storedemo::WriteChunkResponse`
- 新增 client 测试，覆盖成功/幂等/冲突/校验失败/过载/超时/取消/不可用/非法请求状态映射，以及二进制 payload 通过真实 service 的兼容性
- 新增有限自动重试：只对 retryable 状态重试，非 retryable 结果不重试

## StorageNodeClient::WriteChunk 字段映射和状态映射

- local request -> proto request：
  - `request_id`
  - `chunk_id`
  - `object_id`
  - `version`
  - `chunk_index`
  - `offset`
  - `expected_size`
  - `expected_checksum`
  - `payload`
  - `timeout_ms`
  - `best_effort_cancel`
  - `durability`
- proto/gRPC -> local status：
  - proto `OK` -> `kOk`
  - proto `ALREADY_EXISTS` -> `kAlreadyExists`
  - proto `CONFLICT` -> `kConflict`
  - proto `CHECKSUM_MISMATCH` -> `kChecksumMismatch`
  - proto `TIMEOUT` -> `kTimeout`
  - proto `CANCELLED` -> `kCancelled`
  - proto `OVERLOADED` -> `kOverloaded`
  - proto `NODE_UNAVAILABLE` -> `kNodeUnavailable`
  - proto `INVALID_ARGUMENT` -> `kInvalidArgument`
  - proto `IO_ERROR` -> `kIoError`
  - proto `UNSUPPORTED` -> `kUnsupported`
  - gRPC `DEADLINE_EXCEEDED` -> `kTimeout`
  - gRPC `CANCELLED` -> `kCancelled`
  - gRPC `UNAVAILABLE` -> `kNodeUnavailable`
  - gRPC `RESOURCE_EXHAUSTED` -> `kOverloaded`
  - gRPC `INVALID_ARGUMENT` -> `kInvalidArgument`
  - gRPC `UNIMPLEMENTED` -> `kUnsupported`
- `summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 都会映射回本地 response
- 当前自动重试只对 retryable 状态生效：`kTimeout`、`kIoError`、`kOverloaded`、`kNodeUnavailable`
- `kConflict`、`kChecksumMismatch`、`kInvalidArgument`、`kCancelled` 等非 retryable 结果不会自动重试

## deadline / cancellation 当前语义

- `timeout_ms` 会同时写入 proto request，并在 client 侧转换成 gRPC `ClientContext` deadline
- 当前 deadline 是整次 `WriteChunk` 调用的绝对预算；如果配置了重试，每次重试共用同一个绝对 deadline
- `best_effort_cancel` 当前只作为 proto 字段透传，不伪装成已经具备 end-to-end 运行中取消传播
- 真实运行中 cancellation 传播仍受 T019/T020 约束

## 是否调用 metadata / Raft；是否决定 object committed 可见性

- 不调用 metadata
- 不调用 Raft
- 不决定 object committed 可见性

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_client|storage_node_service|write_chunk_contract" --output-on-failure`
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

- 本任务只新增 gRPC client adapter 和测试，没有新增 Windows 专属文件逻辑
- 因此本次不新增 `T032-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T032

- 是

## 是否可以进入 T033

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- `best_effort_cancel` 目前仍只是字段透传，不是完整的 end-to-end cancellation propagation
- upload coordinator / metadata commit gate 仍未实现，T027 orphan chunk 风险继续保留

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/node/module-notes.md`
- 更新了 `modules/store/node/AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T032 标记完成，并同步记录真实修改落点

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：T028，T031/T032 已完成 service/client contract 接入和 deadline/cancelled/error mapping 收口
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
