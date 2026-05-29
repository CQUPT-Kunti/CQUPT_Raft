# T029 Storage Node Proto MVP

## 修改文件

- `CMakeLists.txt`
- `proto/storage_node.proto`
- `tests/no_kv_surface_audit.cmake`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 将 `contracts/storage-node.proto.draft` 收敛为真实 `proto/storage_node.proto`
- 只落 MVP `WriteChunk` RPC schema，不新增 `ReadChunk` / `DeleteChunk` / heartbeat / repair 等后续接口
- 保持 `raft.proto`、`metadata.proto` 和现有 proto target 边界不变
- 在 `CMakeLists.txt` 做 schema-only 路径登记，明确 `proto/storage_node.proto` 已进入源码边界，但不加入 `PROTO_FILES` / `GRPC_PROTO_FILES`
- 最小修正 `tests/no_kv_surface_audit.cmake`，让新 storage proto 在当前 audit 规则下被覆盖并允许静态验证通过，不提前实现 T030 的 codegen target

## proto schema 覆盖的 MVP WriteChunk 字段和状态语义

- `StorageNodeStatusCode`
  - 覆盖 `OK`、`ALREADY_EXISTS`、`NOT_FOUND`、`CONFLICT`、`CHECKSUM_MISMATCH`、`CORRUPTED`、`DISK_FULL`、`PERMISSION_DENIED`、`IO_ERROR`、`TIMEOUT`、`CANCELLED`、`OVERLOADED`、`NODE_UNAVAILABLE`、`UNSUPPORTED`、`INVALID_ARGUMENT`
- `StorageChecksumAlgorithm`
  - 当前固定 `SHA256` 映射位
- `StorageChunkState`
  - 覆盖 `STAGING`、`LIVE`、`DELETING`、`DELETED`、`QUARANTINED`、`CORRUPTED`、`MISSING`
- `WriteChunkDurability`
  - 当前提供 `WRITE_CHUNK_DURABILITY_PUBLISH`
- `StorageChunkChecksum`
  - 表达 checksum algorithm、value、size_bytes、computed_at_unix_ms
- `StorageNodeResponseSummary`
  - 表达 status code、message、request_id、node_id、chunk_id、retry_after_ms
- `WriteChunkRequest`
  - 覆盖 `request_id`
  - 覆盖 `chunk_id`
  - 覆盖 `object_id`
  - 覆盖 `version`
  - 覆盖 `chunk_index`
  - 覆盖 `offset`
  - 覆盖 `expected_size`
  - 覆盖 `expected_checksum`
  - 覆盖 `payload`
  - 覆盖 `timeout_ms`
  - 覆盖 `best_effort_cancel`
  - 覆盖 `durability`
- `WriteChunkResponse`
  - 覆盖 `summary`
  - 覆盖 `size`
  - 覆盖 `checksum`
  - 覆盖 `state`
  - 覆盖 `durable`
  - 覆盖 `already_exists`
- `StorageNodeService`
  - 当前只定义 `rpc WriteChunk(WriteChunkRequest) returns (WriteChunkResponse);`

## 是否修改 CMakeLists.txt

- 是
- 修改内容仅为 schema-only 路径登记与注释说明，确保 `proto/storage_node.proto` 被主构建文件显式提及并通过 no-KV 审计
- `storage_node.proto` 的生成 target、链接边界和避免 `raft_proto` 依赖 storage node 生成代码的工作仍留给 T030

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `protoc --proto_path=proto --cpp_out=/tmp/storage_node_proto_check storage_node.proto`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只新增跨平台无关的 proto schema，没有引入平台相关字段、Windows 构建逻辑或文件语义
- 因此不新增 `T029-WIN`

## 是否通过 T029

- 是

## 是否可以进入 T030

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `check-prerequisites.sh` 仍错误指向 `specs/006-remove-kv-metadata-state-machine`
- 当前主构建尚未把 `storage_node.proto` 接入生成 target；T029 之后仍需要 T030-T032 收口 codegen、service/client 映射和端到端回归
- `tests/no_kv_surface_audit.cmake` 原本对 schema-only storage proto 的覆盖依赖不够显式；本次只做最小修正以通过 T029 静态验证，没有扩展审计范围到 T029 之外的历史问题

## 是否修正了高频文档，为什么

- 是，更新了 `tasks.md`
- 原因：将 T029 标记完成

## 是否更新 module-notes.md / AGENTS.md / contract 文档

- 未更新 `module-notes.md`
- 未更新 `AGENTS.md`
- 未更新 `contracts/storage-node-api.md`
- 未更新 `contracts/storage-node.proto.draft`

## common-risk-notes.md 读取结果

- 已读取
- T027、T028、T019、T014、T023、T025、T026 等现有风险仍存在，T029 不足以关闭

## common-risk-notes.md 新增/删除/保留情况

- 新增：T029 proto 已落 schema 但尚未接入 codegen/service/client 的边界风险
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T028
