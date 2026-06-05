# T044 Storage Node Client ReadChunk

## 修改文件

- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/module-notes.md`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t044-storage-node-client-read-chunk.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `StorageNodeClient` 中新增生产 `ReadChunk` 调用入口和 `StorageNodeClientReadChunkOptions`。
- 新增本地 `storedemo::ReadChunkRequest` 到 `storage::ReadChunkRequest` 的字段转换 helper。
- 新增 `storage::ReadChunkResponse` / gRPC status 到本地 `storedemo::ReadChunkResponse` 的状态、identity、checksum、payload、state 映射 helper。
- 抽出 client deadline helper，复用到现有 `WriteChunk`，并用于 `ReadChunk` 设置 `ClientContext` deadline。
- 扩展 `tests/storage_node_client_test.cpp`，覆盖 fake stub 字段映射、gRPC 错误映射、range boundary、checksum mismatch、non-live 错误和真实 service 二进制读链路。
- 更新 `modules/store/node/module-notes.md`，补充 `storage_node_client.cpp` 中新增的 read helper、deadline helper 和 `StorageNodeClient::ReadChunk` 流程说明。

## StorageNodeClient::ReadChunk 字段映射和状态映射，要求说明是如何从 proto、本地类型和 T043 service 推导的

- 推导来源
  - `proto/storage_node.proto` 定义了 `storage::ReadChunkRequest/Response` 的线协议字段集合。
  - `modules/store/chunk/chunk_store.h` 定义了本地 `storedemo::ReadChunkRequest/Response` 结构：本地请求只具备 `request_id`、`chunk_id`、`range`、`expected_checksum`、`verify_checksum`，没有 `object_id/version/chunk_index`。
  - T043 已固定 `StorageNodeService::ReadChunk` 语义：service 用 `summary.code/message/retry_after_ms`、`chunk_id`、`payload`、`size`、`checksum`、`state`、`offset`、`complete/full_read` 表达读结果。
- request 映射
  - `request_id` -> `storage::ReadChunkRequest.request_id`
  - `chunk_id` -> `storage::ReadChunkRequest.chunk_id`
  - `range.offset/length` -> `offset/length`
  - `expected_checksum` -> `expected_checksum`
  - `verify_checksum` -> `verify_checksum`
  - `options.context.timeout_ms` -> `timeout_ms`
  - `options.context.best_effort_cancel` -> `best_effort_cancel`
  - 本地 read request 没有 `object_id/version/chunk_index`，因此 client 不伪造对象身份字段，只走 chunk-id 路径；这和本地类型边界以及 T043 service 的“chunk_id 优先”语义一致。
- response 映射
  - `summary.code` -> 本地 `status`
  - `summary.message` -> 本地 `error_detail`
  - `summary.retry_after_ms` -> 本地 `retry_after_ms`
  - `summary.node_id` -> `metadata.node_id`
  - 顶层 `chunk_id` 优先，回退 `summary.chunk_id`，再回退本地 request `chunk_id`；解析后恢复 `metadata.identity`
  - `offset` -> `metadata.identity.offset`
  - `size` -> `metadata.size`
  - `state` -> `metadata.state`
  - `payload` -> 本地 `payload`
  - proto `checksum` 同时回填到本地 `metadata.checksum` 和 `actual_checksum`
  - `verified` 由当前可观测语义推导：当 checksum 有值且结果为 `ok` 或 `checksum_mismatch` 时标记为 `true`
- `complete/full_read` 语义处理
  - 本地 `storedemo::ReadChunkResponse` 没有显式 `complete/full_read` 字段，因此 client 不能静默丢失这层 contract。
  - 如果 proto 返回 `status=OK` 但 `complete=false`，client 会显式收口为 `IO_ERROR`。
  - 如果本地请求不是 range read，但 proto 返回 `full_read=false`，client 会显式收口为 `IO_ERROR`。
  - 如果本地请求是 range read，但 proto 返回 `full_read=true`，client 会显式收口为 `IO_ERROR`。
  - 这样可以保持 T041/T043 固定下来的“不能 silent partial success”边界。
- gRPC status 映射
  - `DEADLINE_EXCEEDED` -> `timeout`
  - `CANCELLED` -> `cancelled`
  - `UNAVAILABLE` -> `node unavailable`
  - `INVALID_ARGUMENT` -> `invalid argument`
  - `INTERNAL/UNKNOWN/DATA_LOSS` 等 -> `io error`

## deadline / cancellation 当前边界

- `ReadChunk` 会把 `options.context.timeout_ms` 转成 gRPC `ClientContext` deadline。
- `best_effort_cancel` 会透传到 proto request。
- 当前 client 只保证 RPC 生命周期上的 deadline 约束，不伪造 service/store 端已经具备运行中取消传播。
- T044 不实现 read retry、不实现 read fallback、不实现 replica selection。

## 是否调用 metadata / Raft；是否决定 object committed 可见性

- 调用 metadata：否
- 调用 Raft：否
- 是否决定 object committed 可见性：否

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_client|storage_read_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t044-storage-node-read-client.log`
  - PASS
  - 日志路径：`tmp/007/t044-storage-node-read-client.log`

## Windows 验证判断

- 本任务只新增 client adapter 和平台无关测试，没有新增 Windows 专属行为。
- 当前没有 Windows 编译/测试环境，不伪造 Windows PASS。
- 本任务未新增 `T044-WIN`。

## 是否通过 T044

- 是

## 是否可以进入 T045

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- 本地 `storedemo::ReadChunkResponse` 没有显式 `complete/full_read` 字段，因此 client 只能通过显式一致性校验来保留这层 contract，不能把 proto 成功但语义不完整的响应默默降级成成功。
- `StorageNodeClient::ReadChunk` 已完成，但 read replica selection / fallback 仍未实现。
- `LocalDiskChunkStore::ReadChunk()` 当前 checksum mismatch / corrupted 只返回明确错误，不自动回写 `CORRUPTED` / `QUARANTINED`。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。
- T019/T020 timeout/cancellation、Windows 待验证、restart rebuild / staging cleanup 风险仍存在。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/node/module-notes.md`
- 未更新 `modules/store/node/AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补充：
  - `ResolveAbsoluteDeadline(...)`
  - `ApplyDeadlineToContext(...)`
  - `FillProtoReadRequest(...)`
  - `ResolveReadResponseIdentity(...)`
  - `TranslateProtoReadResponse(...)`
  - `MakeGrpcReadFailureResponse(...)`
  - `StorageNodeClient::ReadChunk(...)`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T044 标记完成，并把描述收紧为 client 输入/输出结构，不宣称已完成 fallback
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：删除过时的“client 未实现”描述，并保留 T044 后仍存在的 read replica selection / fallback 与 corruption 状态自动回写边界
- 修改了 `modules/store/node/module-notes.md`
  - 原因：补充 `StorageNodeClient::ReadChunk` 的关键 helper、输入输出和 deadline/cancellation 边界

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：T044
- 删除：T043
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：将 ReadChunk 剩余风险从 “service 已完成、client/select 待实现” 收紧为 “service/client 已完成，read replica selection / fallback 和 corruption 状态自动回写仍待后续任务”
