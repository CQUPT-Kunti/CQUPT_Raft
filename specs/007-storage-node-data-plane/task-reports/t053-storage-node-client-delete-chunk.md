# T053 StorageNodeClient Delete Chunk

## 修改文件

- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/module-notes.md`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t053-storage-node-client-delete-chunk.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `StorageNodeClient` 新增 `DeleteChunk` / `BatchDeleteChunks` 本地请求、响应和调用入口。
- 实现 delete/batch delete 的 proto request builder、proto response translator、gRPC status mapper、deadline 应用和 batch 聚合事实校验。
- 扩展 `storage_node_client` 测试：
  - fake stub 验证单删/批删字段映射、deadline、gRPC 状态映射、partial batch result 和 retry 分类。
  - 真实 `StorageNodeService + LocalDiskChunkStore` 验证 live delete、checksum mismatch 不误删、metadata 可见性不由 client 决定。

## StorageNodeClient::DeleteChunk / BatchDeleteChunks 字段映射和状态映射，说明如何从 proto、T050 contract、T052 service、本地 ChunkStore 推导

- 单删请求映射：本地请求转到 proto `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`expected_checksum`、`reason`、`metadata_boundary`、`timeout_ms`、`best_effort_cancel`。
- 单删响应映射：从 proto `summary.code/message/retry_after_ms`、`chunk_id`、`size`、`checksum`、`state`、`deleted/already_missing/already_deleted/retryable` 转回本地删除响应，并恢复本地 `ChunkIdentity`。
- 批删请求映射：top-level `request_id/timeout_ms/best_effort_cancel` 原样写入 proto；每个 item 独立映射 `chunk_id/object_id/version/chunk_index/expected_checksum/reason/metadata_boundary`。
- 批删响应映射：每个 `BatchDeleteChunkResult` 先按单删语义转成本地逐项结果，再校验 `success_count`、`idempotent_count`、`retryable_failure_count`、`non_retryable_failure_count`、`partial_failure` 是否与逐项事实一致。
- 状态语义沿用 T050/T052 已固定边界：
  - `missing/deleted` 幂等由 `already_missing` 和 `DELETED` 状态恢复。
  - `retryable/non-retryable` 以 proto 标记和本地 `IsRetriableStatus()` 联合收口。
  - `DEADLINE_EXCEEDED -> timeout`
  - `CANCELLED -> cancelled`
  - `UNAVAILABLE -> node unavailable`
  - 其它非 OK gRPC 失败显式映射为明确错误，不做 silent success。

## BatchDeleteChunks partial result / retry 分类语义

- client 保留每个 item 的独立结果，不把 retryable/non-retryable failure 折叠成单一 top-level 状态。
- top-level 批删响应保留 `success_count`、`idempotent_count`、`retryable_failure_count`、`non_retryable_failure_count`、`partial_failure`，并验证这些聚合事实与逐项结果一致。
- 当前 client 不自动重试 retryable item，只把后续调度所需分类显式返回给调用方。

## deadline / cancellation 当前边界

- `timeout_ms` 会同时写入 proto request，并设置 gRPC `ClientContext` deadline。
- `best_effort_cancel` 当前只做字段透传。
- 当前没有实现 service/store 运行中取消传播，也没有为 delete/batch delete 增加自动重试。

## 是否调用 metadata / Raft；是否决定 object deleted 可见性

- 不调用 metadata / Raft。
- 不决定 object deleted 可见性。
- 真实删除测试验证了 `DeleteChunk` 成功后 metadata `HeadObject/ListObjects` 仍以 metadata committed 事实为准。

## 是否使用 tests/test_file/test_file.zip

- 是。T053 新增/修改的删除相关真实 payload 路径使用 `tests/test_file/test_file.zip`。

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "storage_node_client|storage_delete_chunk_contract|delete_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t053-storage-node-delete-client.log`
  - PASS
  - 日志路径：`tmp/007/t053-storage-node-delete-client.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败。

## Windows 验证判断

- 当前无 Windows 编译/测试环境。
- 本任务未伪造 Windows PASS，也未新增 `T053-WIN`。
- Windows 删除语义风险继续保留到后续任务。

## 是否通过 T053

- 是。

## 是否可以进入 T054

- 可以进入 T054。
- 进入前提是继续保持“只实现生产 GC，不回头扩展 delete client 范围”。

## 当前任务发现的不合理点 / 警告 / 风险

- delete client 已完成，但生产 `GarbageCollector`、restart 后继续 cleanup、Windows 实机删除验证仍未完成。
- `best_effort_cancel` 仍只是字段透传，delete/batch delete 没有 service/store 运行中取消传播。
- 批删 retryable/non-retryable 已能由 client 端到端观察，但当前没有后台调度器消费这些结果。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/node/module-notes.md`。
- 未更新 `modules/store/node/AGENTS.md`。

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是。已补充：
  - delete request 转换 helper
  - delete response 转换 helper
  - batch delete request/result 转换 helper
  - gRPC status 映射 helper
  - deadline / cancellation 处理 helper
  - retryable/non-retryable 分类与 batch 聚合校验 helper
  - `StorageNodeClient::DeleteChunk`
  - `StorageNodeClient::BatchDeleteChunks`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`：标记 T053 完成。
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`：把“client 删除 RPC 未实现”推进为“client 已实现，但 GC/restart cleanup/Windows/cancellation 传播仍待后续任务”。
- 修改了 `modules/store/node/module-notes.md`：记录 T053 新增 client 删除适配和 `.cpp` 关键 helper 边界。

## common-risk-notes.md 读取结果

- 已读取现有风险。
- T019/T020 timeout/cancellation 运行中传播、T024 corrupted 自动回写、T025 Windows 删除语义、T027 pending/orphan cleanup、T045 registry/failure cache、T049/T053 删除后续 GC/restart cleanup 风险仍然存在。

## common-risk-notes.md 新增/删除/保留情况

- 删除：无。
- 新增：无。
- 保留并更新：
  - 原 `T052` 删除链路风险已更新为 `T053`，说明 client 删除 RPC 已实现，但生产 GC、restart cleanup、Windows 删除验证和运行中 cancellation 传播仍未完成。
