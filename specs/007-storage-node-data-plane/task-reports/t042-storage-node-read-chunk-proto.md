# T042 Storage Node ReadChunk Proto

## 修改文件

- `proto/storage_node.proto`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t042-storage-node-read-chunk-proto.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `proto/storage_node.proto` 中新增 `ReadChunkRequest`、`ReadChunkResponse` 和 `StorageNodeService.ReadChunk` RPC。
- 复用现有 `StorageNodeStatusCode`、`StorageChunkChecksum`、`StorageChunkState`、`StorageNodeResponseSummary`，没有新增冲突状态体系。
- 保持 schema 只表达 chunk/data-plane 读请求与结果，不表达 object committed 可见性，不暴露本地文件路径，也不把 object payload 放进 metadata/Raft 语义。
- 为了适配新增 proto RPC 后的 generated `StubInterface` 抽象接口扩展，最小修正了 `tests/storage_node_client_test.cpp` 中的 fake stub，补齐默认 `ReadChunk` 空实现和 async 占位，未实现任何生产 ReadChunk client 逻辑。

## ReadChunk proto 字段和状态语义

- `ReadChunkRequest`
  - `request_id`
  - `chunk_id`
  - `object_id`
  - `version`
  - `chunk_index`
  - `offset`
  - `length`
  - `expected_checksum`
  - `timeout_ms`
  - `best_effort_cancel`
  - `verify_checksum`
- `ReadChunkResponse`
  - `summary`
  - `chunk_id`
  - `payload`
  - `size`
  - `checksum`
  - `state`
  - `offset`
  - `complete`
  - `full_read`
- 状态语义
  - 统一复用 T029 已有 `StorageNodeStatusCode`
  - range read 当前即使只是 schema 预留，也可以通过 `summary.code` 表达 `unsupported` / `invalid_argument`
  - schema 不携带 object state / committed visibility 决策
  - schema 不暴露本地文件路径、staging 路径或 metadata 内部实现细节

## 是否修改 CMakeLists.txt

- 否
- 现有 `storage_node_proto` codegen target 已能自动处理 `proto/storage_node.proto` 变更

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target storage_node_proto 2>&1 | tee tmp/007/t042-storage-node-proto.log`
  - PASS
  - 日志路径：`tmp/007/t042-storage-node-proto.log`
- `protoc --proto_path=proto --cpp_out=/tmp/storage_node_proto_check proto/storage_node.proto 2>&1 | tee -a tmp/007/t042-storage-node-proto.log`
  - PASS
  - 日志路径：`tmp/007/t042-storage-node-proto.log`
- `protoc --proto_path=proto --grpc_out=/tmp/storage_node_grpc_check --plugin=protoc-gen-grpc=$(command -v grpc_cpp_plugin) proto/storage_node.proto 2>&1 | tee -a tmp/007/t042-storage-node-proto.log`
  - PASS
  - 日志路径：`tmp/007/t042-storage-node-proto.log`
- `ctest --test-dir build/linux -R "storage_read_chunk_contract|storage_read" --output-on-failure 2>&1 | tee tmp/007/t042-read-chunk-contract.log`
  - PASS
  - 日志路径：`tmp/007/t042-read-chunk-contract.log`

## Windows 验证判断

- T042 是 proto/schema/codegen 任务，当前没有 Windows 编译/测试环境。
- 未伪造 Windows PASS。
- 本任务没有新增 `T042-WIN`。

## 是否通过 T042

- 是

## 是否可以进入 T043

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- 新增 `ReadChunk` RPC 后，generated `StubInterface` 抽象接口会扩大，任何测试内 fake stub 都需要同步补齐占位实现；这类修正属于 codegen 兼容性维护，不等于生产 client 已实现。
- 当前只完成了 proto/schema/codegen；`StorageNodeService::ReadChunk`、`StorageNodeClient::ReadChunk` 和 read replica selection 仍未实现。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。
- T024 的 corrupted 状态不自动回写、T019/T020 timeout/cancellation、Windows 待验证、restart rebuild / staging cleanup 风险仍存在。

## 是否更新 module-notes.md / AGENTS.md

- 未更新

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 不需要
- 本任务未修改 `modules/store/*` 生产代码

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T042 标记完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：删除已过时的“proto ReadChunk 未实现”描述，并保留 T042 后仍存在的 service/client/read replica selection 风险

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：T042
- 删除：T041
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：将 ReadChunk 剩余风险从 “proto/service/client/select 都未实现” 收紧为 “proto 已完成，service/client/select 仍待后续任务”
