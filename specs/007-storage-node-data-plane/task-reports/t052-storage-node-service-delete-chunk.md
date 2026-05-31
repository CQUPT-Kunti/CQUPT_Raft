# T052 Storage Node Service Delete Chunk

## 修改文件

- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/module-notes.md`
- `tests/storage_node_service_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t052-storage-node-service-delete-chunk.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `StorageNodeService` 中实现生产 `DeleteChunk` gRPC 适配层
- 在 `StorageNodeService` 中实现生产 `BatchDeleteChunks` gRPC 适配层
- 新增 delete request/response 映射 helper、batch 聚合 helper 和 retryable/non-retryable 分类复用
- 扩展 `tests/storage_node_service_test.cpp`，覆盖单删 live/missing/deleted/checksum mismatch/identity mismatch 和 batch partial result 聚合语义
- 更新 `modules/store/node/module-notes.md`，补充 `.cpp` 中新增关键 helper 和流程边界

## StorageNodeService::DeleteChunk / BatchDeleteChunks 字段映射和状态映射，说明如何从 proto、T050 contract、本地 ChunkStore 推导

- `DeleteChunk` 把 proto `request_id`、`chunk_id`、`reason`、`metadata_boundary`、`expected_checksum` 映射到 `ChunkStore::DeleteChunkRequest`
- 如果 proto 没有显式 `chunk_id`，但给了 `object_id + version + chunk_index`，service 会派生 chunk id；如果显式 `chunk_id` 与 object identity 同时出现但不一致，service 直接返回显式参数错误，避免误删 live chunk
- 这个 identity 校验来自 T050 中“不误删”的 contract，以及 T051 在 schema 中保留的 object identity 输入字段
- `DeleteChunkResponse.summary.code/message/retry_after_ms` 直接来自 `ChunkStore::DeleteChunkResponse.status/error_detail/retry_after_ms`
- `DeleteChunkResponse.chunk_id` 优先用 store metadata 的 chunk id；缺失时回退到 request.chunk_id，再必要时从 object identity 派生
- `DeleteChunkResponse.state/size/checksum` 直接映射 store metadata 事实
- `deleted` 直接映射 store `deleted`
- `already_missing` 直接映射 store `already_missing`
- `already_deleted` 由 `already_missing && state == DELETED` 推导，用来区分“确实缺失”和“重复删除已删 chunk”
- `retryable` 不新造状态体系，直接复用本地 `storedemo::IsRetriableStatus()`
- `BatchDeleteChunks` 逐项把 `BatchDeleteChunkRequest` 转成一次 `ChunkStore::DeleteChunkRequest`，每项复用与单删相同的字段和状态映射
- batch item 的本地 `request_id` 由 top-level `request_id` 扩展成 `/item/<index>`，这样既满足底层 store 的非空约束，也保留逐项可诊断性

## BatchDeleteChunks partial result / retry 分类语义

- `BatchDeleteChunks` 按请求顺序逐项调用 `ChunkStore::DeleteChunk()`，每个 chunk 返回独立 `BatchDeleteChunkResult`
- `success_count` 只统计本次真正删除成功的项
- `idempotent_count` 统计 `status == OK` 且 `already_missing/already_deleted` 的幂等成功项
- `retryable_failure_count` 统计 `status != OK` 且 `IsRetriableStatus(status)` 的失败项
- `non_retryable_failure_count` 统计其余失败项
- `partial_failure` 只在“既有成功/幂等成功，又有失败”时置为 `true`
- top-level `summary` 只表达 batch RPC 已执行以及是否存在 item failure；真正的重试决策应以逐项 `results` 为准

## 是否调用 ChunkStore；是否调用 metadata / Raft

- 调用 `ChunkStore`
  - 是，`DeleteChunk` 只调用注入的 `ChunkStore::DeleteChunk`
  - 是，`BatchDeleteChunks` 只做多次 `ChunkStore::DeleteChunk`
- 调用 metadata / Raft
  - 否，不调用 metadata service、`MetadataStateMachine` 或 `RaftNode::ProposeMetadata()`

## 是否使用 tests/test_file/test_file.zip

- 是，T052 新增和修改的删除路径 service 测试主 fixture 使用 `tests/test_file/test_file.zip`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存
- `ctest --test-dir build/linux -R "storage_node_service|storage_delete_chunk_contract|delete_chunk_contract" --output-on-failure 2>&1 | tee tmp/007/t052-storage-node-delete-service.log`
  - PASS
  - 日志路径：`tmp/007/t052-storage-node-delete-service.log`
  - 说明：真实匹配到的测试名为 `storage_delete_chunk_contract` 和 `storage_node_service`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证最终 PASS，无失败项

## Windows 验证判断

- T052 当前只在 Linux 环境完成 service adapter 和相关测试验证
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T052-WIN`

## 是否通过 T052

- 是

## 是否可以进入 T053

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `tasks.md` 原 T052 仍包含 `StatChunk` / `ListChunks`，但本轮按当前任务边界只实现 `DeleteChunk` / `BatchDeleteChunks` service adapter；已按实际落点收口
- 当前 top-level `BatchDeleteChunksResponse.summary` 只表达 batch RPC 执行结果，真正的 retry 决策仍应以逐项 `results` 为准
- 当前只实现了 service 侧删除 RPC，不代表 `StorageNodeClient::DeleteChunk`、生产 GC、restart cleanup 或 Windows 删除语义已完成

## 是否更新 module-notes.md / AGENTS.md

- `module-notes.md`
  - 是，已更新
- `AGENTS.md`
  - 否，本任务没有新增模块级规则需求

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是，已补充：
  - DeleteChunk request 转换 helper
  - DeleteChunk response 填充 helper
  - BatchDeleteChunks 聚合 helper
  - retryable/non-retryable 分类 helper
  - `StorageNodeService::DeleteChunk` 流程
  - `StorageNodeService::BatchDeleteChunks` 流程

## 是否修改高频文档及原因

- 是，修改了 `tasks.md`
  - 原因：把 T052 的任务项收口到本轮真实实现范围，避免把未做的 `StatChunk` / `ListChunks` 混在已完成状态里

## common-risk-notes.md 读取结果

- 已读取
- 当前与 T052 直接相关的删除路径风险主要是：
  - client 删除 RPC 仍未实现
  - 生产 GC / restart cleanup 仍未实现
  - Windows 删除行为仍未实机验证

## common-risk-notes.md 新增/删除/保留情况

- 删除/替换
  - 将 “T051 proto 已有但生产 service 未实现” 风险推进为 “T052 service 已实现，但 client/GC/restart cleanup/Windows 仍未完成”
- 保留
  - T049 的 GC / restart cleanup 风险
  - T025 / T026 的 Windows 删除与并发验证风险
  - 其它与 timeout/cancellation、corruption 状态回写、registry/failure cache 相关风险
