# store/upload 说明

## 模块职责

`modules/store/upload` 承载最小 upload coordinator / helper。

当前只负责：

- `CreateObject -> Placement -> WriteChunk -> CommitObject` 的协调顺序
- 调用 `PlacementManager` 选择候选副本节点
- 调用抽象 `UploadChunkWriter` 写入 chunk bytes
- 调用抽象 `UploadMetadataClient` 创建 pending object 并提交 manifest
- 在结果中显式暴露 `pending_object_possible` 和 `orphan_chunk_possible`

当前不负责：

- GC / Abort / cleanup scheduler
- Repair / Rebalance / Scrub
- heartbeat / registry
- Raft 提案逻辑
- 真实 metadata gRPC client 或真实 StorageNode registry

## 主要结构

### `UploadChunkInput`

- 描述单个待上传 chunk 的输入
- 字段：
  - `chunk_index`
  - `offset`
  - `payload`
  - `expected_size`
  - `expected_checksum`

### `UploadCommittedChunk`

- 描述最终进入 metadata manifest 的单个 chunk facts
- 字段：
  - `identity`
  - `offset`
  - `size`
  - `checksum`
  - `replica_nodes`

### `UploadReplicaWriteResult`

- 描述一次对单个 StorageNode 的 `WriteChunk` 结果
- 字段：
  - `node_id`
  - `status`
  - `error_detail`
  - `retry_after_ms`
  - `durable`
  - `already_exists`
  - `metadata`

### `UploadChunkExecution`

- 汇总某个 chunk 的 placement 和写入尝试
- 字段：
  - `identity`
  - `placement_decision`
  - `replica_results`
  - `durable_success_count`
  - `commit_eligible`

### `UploadCoordinatorRequest`

- 一次对象上传的输入
- 字段：
  - `request_id`
  - `bucket`
  - `object_key`
  - `object_id`
  - `version`
  - `etag`
  - `chunks`
  - `replica_policy`
  - `candidates`
  - `excluded_nodes`
  - `context`
  - `client_time_unix_ms`

### `UploadCoordinatorResult`

- 一次对象上传的最终结果
- 字段：
  - `status`
  - `error_detail`
  - `create_succeeded`
  - `committed`
  - `pending_object_possible`
  - `orphan_chunk_possible`
  - `committed_chunks`
  - `chunk_executions`

## 抽象边界

### `UploadMetadataClient`

- 负责 metadata/control-plane 侧的 `CreateObject` 和 `CommitObject`
- 当前 coordinator 只依赖这个抽象，不直接依赖 metadata gRPC service、`MetadataStateMachine` 或 `RaftNode`

### `UploadChunkWriter`

- 负责把单个 chunk 写到某个目标 StorageNode
- 当前 coordinator 只依赖这个抽象，不直接依赖真实 `StorageNodeClient`
- 测试中可以用 test adapter，后续可以接真实 `StorageNodeClient`

## `UploadCoordinator::UploadObject()` 语义

- 执行顺序：
  - 校验请求
  - `CreateObject`
  - 对每个 chunk 执行 `PlacementManager::SelectPlacement()`
  - 对 placement 结果中的每个 replica node 调用 `UploadChunkWriter::WriteChunkToNode()`
  - 每个 chunk 至少达到 `minimum_successful_writes` 个 durable 成功后，才允许进入 manifest
  - 全部 chunk 满足条件后，调用 `CommitObject`
- 成功语义：
  - `status == kOk`
  - `create_succeeded == true`
  - `committed == true`
  - `pending_object_possible == false`
  - `orphan_chunk_possible == false`
- 失败语义：
  - `WriteChunk` 不足以达到 `minimum_successful_writes` 时，不调用 `CommitObject`
  - `CommitObject` 失败时，durable chunk 仍可能存在，因此 `orphan_chunk_possible == true`
  - `CreateObject` 成功但后续 placement / write / commit 失败时，`pending_object_possible == true`

## `upload_coordinator.cpp` 实现细节

### 匿名命名空间内部 helper

### `JoinChunkRequestId(std::string_view, std::string_view)`

- 作用：
  - 为 `CreateObject`、每个 replica 的 `WriteChunk`、`CommitObject` 生成派生 request id。
- 当前格式：
  - `base_request_id + "/" + suffix`
- 用途：
  - 让同一次 upload 内的 metadata / chunk 子操作有稳定可追踪的 request id。

### `ValidateUploadRequest(const UploadCoordinatorRequest&, std::string*)`

- 作用：
  - 在真正触发 metadata / placement / write 前，先做 upload 级别参数校验。
- 当前检查项：
  - `request_id` 不能为空
  - `bucket` 不能为空
  - `object_key` 不能为空
  - `object_id` 不能为空
  - `version > 0`
  - `chunks` 不能为空
  - 每个 chunk 的 `payload` 不能为空
  - 如果给了 `expected_size`，它必须与 `payload.size()` 一致
- 返回语义：
  - 合法时返回 `kOk`
  - 非法时返回 `kInvalidArgument`，并把人类可读原因写入 `error_detail`

### `ResolveChunkIdentity(const UploadCoordinatorRequest&, const UploadChunkInput&, ChunkIdentity*, std::string*)`

- 作用：
  - 为单个 chunk 生成最终使用的 `ChunkIdentity`
- 当前逻辑：
  - 用 `object_id + version + chunk_index` 调 `MakeChunkId(...)`
  - 同时回填 `offset`
- 返回语义：
  - 成功返回 `kOk`
  - `MakeChunkId(...)` 失败时透传对应错误

### `ResolveExpectedChecksum(const UploadChunkInput&, ChunkChecksum*, std::string*)`

- 作用：
  - 确认单个 chunk 的期望 checksum
- 当前逻辑：
  - 如果调用方已经给了 `expected_checksum`，直接使用
  - 否则用 `ComputeChunkChecksum(payload, ...)` 现算
- 说明：
  - 这样 coordinator 不要求每个测试或上层调用方都手工先算 checksum

### `ResolveExpectedSize(const UploadChunkInput&)`

- 作用：
  - 决定单个 chunk 写入请求里使用的 `expected_size`
- 当前逻辑：
  - 优先使用调用方传入的 `expected_size`
  - 否则退回 `payload.size()`

### `ComputeObjectEtag(const UploadCoordinatorRequest&, std::string*, std::string*)`

- 作用：
  - 决定 `CreateObject` / `CommitObject` 里使用的对象级 `etag`
- 当前逻辑：
  - 如果请求已经带了 `etag`，直接使用
  - 否则把所有 chunk payload 按输入顺序拼接后，调用 `ComputeChunkChecksum(...)` 生成一个对象级摘要字符串
- 边界：
  - 这里只生成 metadata 里的 `etag`，不会把 payload 写进 metadata 或 Raft

### `ComputeObjectSize(const UploadCoordinatorRequest&)`

- 作用：
  - 统计对象总大小
- 当前逻辑：
  - 累加所有 `chunk.payload.size()`

### `IsDurableWriteSuccess(const WriteChunkResponse&)`

- 作用：
  - 判断一次 replica 写入能否计入“durable success”
- 当前判定：
  - `status == kOk` 或 `status == kAlreadyExists`
  - 且 `durable == true`
- 含义：
  - `already_exists + durable` 被视为可复用的成功副本

### `ResolveUploadFailureStatus(const std::vector<UploadReplicaWriteResult>&)`

- 作用：
  - 当某个 chunk 没达到 `minimum_successful_writes` 时，为整个 upload 选一个更有诊断意义的失败状态
- 当前优先级：
  - `kChecksumMismatch`
  - `kConflict`
  - `kOverloaded / kTimeout / kCancelled / kNodeUnavailable / kIoError`
  - 以上都没有时，兜底 `kNodeUnavailable`

### `ResolveUploadFailureDetail(const ChunkIdentity&, const std::vector<UploadReplicaWriteResult>&, std::size_t)`

- 作用：
  - 生成某个 chunk 未达到最小成功副本数时的错误详情
- 当前逻辑：
  - 优先复用第一个非成功 replica 的 `error_detail`
  - 如果没有更具体的底层消息，就返回一个包含 `chunk_id` 和 `minimum_successful_writes` 的通用错误文本

### `MakeReplicaWriteResult(std::string, const WriteChunkResponse&)`

- 作用：
  - 把底层 `WriteChunkResponse` 压缩成 coordinator 结果里的 `UploadReplicaWriteResult`
- 当前保留字段：
  - `node_id`
  - `status`
  - `error_detail`
  - `retry_after_ms`
  - `durable`
  - `already_exists`
  - `metadata`

## `UploadCoordinator::UploadObject()` 的实际执行流程

### 1. 请求预校验

- 先调用 `ValidateUploadRequest(...)`
- 如果失败，直接返回，不会调用 metadata、placement 或 chunk writer

### 2. 计算对象级 metadata facts

- 先通过 `ComputeObjectSize(...)` 统计对象总大小
- 再通过 `ComputeObjectEtag(...)` 决定要写进 metadata 的 `etag`

### 3. 先创建 pending object

- 调用 `metadata_client_->CreateObject(...)`
- 使用派生 request id：`<request_id>/create`
- 如果失败：
  - 直接返回
  - `create_succeeded == false`
  - 不会进入 placement / write / commit
- 如果成功：
  - `create_succeeded = true`
  - `pending_object_possible = true`
- 这里把 `pending_object_possible` 先置真，是因为只要 create 成功，后续失败就会留下 pending object 风险，直到最终 commit 成功才清零

### 4. 逐 chunk 执行 placement 和写入

- 对 `request.chunks` 逐个处理
- 每个 chunk 会在 `result.chunk_executions` 中留下一条完整执行记录

### 5. 为每个 chunk 解析 identity

- 调 `ResolveChunkIdentity(...)`
- 失败直接返回，不进入 placement

### 6. 调用 `PlacementManager`

- coordinator 为每个 chunk 构造一个 `PlacementRequest`
- 关键映射：
  - `identity`
  - `chunk_size_bytes = ResolveExpectedSize(chunk)`
  - `policy = request.replica_policy`
  - `excluded_nodes = request.excluded_nodes`
  - `decision_epoch = request.client_time_unix_ms`
- `placement_decision` 无论成功失败都会保留到 `chunk_execution`
- 如果 placement 失败：
  - 直接返回
  - 不会调用 chunk writer
  - 此时通常 `orphan_chunk_possible == false`

### 7. 解析单 chunk 的期望 checksum

- 调 `ResolveExpectedChecksum(...)`
- 如果失败，直接返回

### 8. 对 placement 选中的 replica 逐个写入

- 对 `placement_decision.replica_nodes` 逐个调用 `chunk_writer_->WriteChunkToNode(...)`
- 每次子写入的 request id 形如：
  - `<request_id>/write-<chunk_id>-<node_id>`
- 每次写入都会记录到 `chunk_execution.replica_results`

### 9. durable success 判定和副本去重

- 只有 `IsDurableWriteSuccess(...)` 为真时，才会计入 durable success
- 第一个 durable success 的返回值会被缓存为 `first_durable_response`
  - 后续用于回填 `size` 和 `checksum`
- `durable_replicas` 会按 `node_id` 去重，避免重复副本节点进入 manifest

### 10. 最小成功副本数判断

- `chunk_execution.durable_success_count = durable_replicas.size()`
- `chunk_execution.commit_eligible` 判断条件：
  - `durable_success_count >= placement_decision.minimum_successful_writes`
- 如果某个 chunk 不满足：
  - 整个 upload 立即失败
  - 不会调用 `CommitObject`
  - `status` 通过 `ResolveUploadFailureStatus(...)` 选取
  - `error_detail` 通过 `ResolveUploadFailureDetail(...)` 生成
  - 如果已有 durable chunk，`orphan_chunk_possible = true`

### 11. 组装 `committed_chunks`

- 对每个满足条件的 chunk，组装一个 `UploadCommittedChunk`
- 当前填充规则：
  - `identity` 来自 chunk identity
  - `offset` 来自 `UploadChunkInput.offset`
  - `size`
    - 优先取 `first_durable_response.metadata.size`
    - 如果底层没填 size，则退回 `ResolveExpectedSize(chunk)`
  - `checksum`
    - 优先取 `first_durable_response.metadata.checksum`
    - 如果底层没填 checksum，则退回 `expected_checksum`
  - `replica_nodes` 使用所有 durable 成功副本

### 12. 按 offset / chunk_index 排序 manifest

- 在调用 `CommitObject` 之前，`committed_chunks` 会排序
- 当前排序键：
  - `offset`
  - 然后 `chunk_index`
- 目的是确保写进 metadata 的 manifest 顺序稳定

### 13. 提交 metadata manifest

- 调用 `metadata_client_->CommitObject(...)`
- 使用派生 request id：`<request_id>/commit`
- 提交内容包括：
  - `bucket`
  - `object_key`
  - `object_id`
  - `version`
  - `size`
  - `etag`
  - `chunks = committed_chunks`

### 14. commit 成功或失败后的最终状态

- `CommitObject` 失败时：
  - 返回 metadata client 给出的失败状态
  - `error_detail` 形如 `CommitObject failed: ...`
  - `orphan_chunk_possible = !committed_chunks.empty()`
  - `pending_object_possible` 仍保持 `true`
- `CommitObject` 成功时：
  - `status = kOk`
  - `error_detail` 清空
  - `committed = true`
  - `pending_object_possible = false`
  - `orphan_chunk_possible = false`

## 当前边界

- 当前 helper 不实现 `AbortObject`
- 当前 helper 不实现 background cleanup
- 当前 helper 不实现 retry scheduler
- 当前 helper 不做多轮重新 placement
- 当前 helper 不把 payload 写进 metadata / Raft，只把 `UploadCommittedChunk` 转成 manifest facts
