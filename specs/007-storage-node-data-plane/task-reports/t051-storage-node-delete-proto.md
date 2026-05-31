# T051 Storage Node Delete Proto

## 修改文件

- `proto/storage_node.proto`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t051-storage-node-delete-proto.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `proto/storage_node.proto` 中新增 `DeleteChunkRequest` / `DeleteChunkResponse`
- 在 `proto/storage_node.proto` 中新增 `BatchDeleteChunkRequest` / `BatchDeleteChunkResult` / `BatchDeleteChunksRequest` / `BatchDeleteChunksResponse`
- 在 `StorageNodeService` 中新增 `DeleteChunk` 和 `BatchDeleteChunks` RPC
- 复用现有 `StorageNodeStatusCode`、`StorageNodeResponseSummary`、`StorageChunkState`、`StorageChunkChecksum`，没有新增冲突状态体系
- 因生成的 `StorageNodeService::StubInterface` 扩展了删除 RPC，仅在 `tests/storage_node_client_test.cpp` 同步最小 fake stub，未实现任何生产删除逻辑

## DeleteChunk / BatchDeleteChunks proto 字段和状态语义，说明如何从 T050 contract 和本地 ChunkStore 推导

- 单删请求字段以 `chunk_id` 为主键，补充 `object_id`、`version`、`chunk_index`、`expected_checksum`、`reason`、`metadata_boundary`、`timeout_ms`、`best_effort_cancel`
- 其中 `chunk_id + expected_checksum` 直接对应 `ChunkStore::DeleteChunkRequest` 和 `LocalDiskChunkStore::DeleteChunk()` 当前真实依赖；`reason` / `metadata_boundary` 也与本地 store 请求结构保持一致
- `object_id`、`version`、`chunk_index` 延续 `WriteChunk` / `ReadChunk` 现有 schema 风格，用于后续 service/client 在不改变 chunk data-plane 边界的前提下表达 identity 校验输入，固定 T050 中“不误删”的 contract 方向
- 单删响应复用 `summary` 承载状态码、消息、`retry_after_ms`，并补充 `state`、`deleted`、`already_missing`、`already_deleted`、`retryable`、`size`、`checksum`
- 这样可以覆盖 T050 固定的 live 删除成功、missing/deleted 幂等、checksum mismatch 明确失败，以及 retryable / non-retryable 失败分类
- 批删请求按 `repeated BatchDeleteChunkRequest chunks` 表达多个 chunk，避免把 object 可见性语义塞进 data-plane 删除协议
- 批删响应按 `repeated BatchDeleteChunkResult results` 返回每个 chunk 的独立结果，并用 `success_count`、`idempotent_count`、`retryable_failure_count`、`non_retryable_failure_count`、`partial_failure` 固定 partial batch result 语义
- 整体 schema 只表达 chunk 删除，不表达 object 是否 deleted；对象可见性仍由 metadata 决定

## 是否使用 tests/test_file/test_file.zip

- 否；本任务只做 proto/schema/codegen 和 fake stub 同步，没有新增或修改依赖真实 payload 的删除测试

## 是否修改 CMakeLists.txt

- 否；现有 `storage_node_proto` target 已自动接住本次 proto 变更

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存
- `cmake --build --preset debug-ninja-low-parallel --target storage_node_proto 2>&1 | tee tmp/007/t051-storage-node-delete-proto.log`
  - PASS
  - 日志路径：`tmp/007/t051-storage-node-delete-proto.log`
- `protoc --proto_path=proto --cpp_out=/tmp/storage_node_proto_check proto/storage_node.proto 2>&1 | tee -a tmp/007/t051-storage-node-delete-proto.log`
  - PASS
  - 日志路径：`tmp/007/t051-storage-node-delete-proto.log`
- `protoc --proto_path=proto --grpc_out=/tmp/storage_node_grpc_check --plugin=protoc-gen-grpc=$(command -v grpc_cpp_plugin) proto/storage_node.proto 2>&1 | tee -a tmp/007/t051-storage-node-delete-proto.log`
  - PASS
  - 日志路径：`tmp/007/t051-storage-node-delete-proto.log`
- `ctest --test-dir build/linux -R "storage_delete_chunk_contract|delete_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t051-delete-contract.log`
  - PASS
  - 日志路径：`tmp/007/t051-delete-contract.log`
  - 说明：真实匹配到的测试名为 `storage_delete_chunk_contract`

## 如果失败：失败原因、失败检查/测试名、错误摘要、最后 50 行日志摘要

- 本次验证最终 PASS，无失败项

## Windows 验证判断

- T051 当前只在 Linux 环境完成 proto/schema/codegen 验证
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T051-WIN`；Windows codegen 和删除路径行为仍待后续环境验证

## 是否通过 T051

- 是

## 是否可以进入 T052

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `tasks.md` 原 T051 描述仍包含 `StatChunk` / `ListChunks` MVP 字段，但本轮按当前任务边界只实现 `DeleteChunk` / `BatchDeleteChunks` proto/schema/codegen；已在任务项中按实际落点收口
- 本次只补齐删除 proto 和 codegen，不代表生产 `StorageNodeService::DeleteChunk`、`StorageNodeClient::DeleteChunk`、真实批删执行路径或生产 GC 已完成
- partial batch result 的 retryable / non-retryable 分类目前已在 contract test 和 schema 层固定，仍需 T052-T053 的真实 service/client 映射继续验证
