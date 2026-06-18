# transfer 模块说明

## 模块职责

`modules/store/transfer/` 负责 `storage_client` 侧真实 upload/download 的客户端编排边界。它连接：

- ViewNode discovery / observation
- MetadataNode metadata control-plane
- StorageNode data-plane

但它不成为新的 metadata authority，也不让真实 payload 进入 Raft。

当前已经实现的能力主要有：

- `ObjectTransfer` upload/download 会话
- bounded 文件 chunk reader
- 增量 checksum state
- Metadata transfer client
- Storage transfer client
- ViewNode discovery integration
- manifest-driven download reconstruction
- Metadata `NOT_LEADER` leader hint retry boundary
- StorageNode transient retry/backoff
- failed upload cleanup candidate emission
- session 级 bounded concurrency budget

## 当前关键结构和接口

### `object_transfer.h`

当前对外边界主要包括：

- `ObjectTransfer`
  - 创建 upload/download session
- `TransferSessionSnapshot`
  - 记录 request、stage、bytes/chunks progress、failure、最终 checksum/commit 状态
- `UploadObjectRequest` / `DownloadObjectRequest`
  - 显式带 `cluster_id`，强制 discovery 范围清晰
- `TransferChunkReader`
  - 只做 bounded 文件 chunk 读取
- `TransferChecksumState`
  - 只做增量 checksum
- metadata facts 类型：
  - `TransferPreparedChunk`
  - `TransferWritePlan`
  - `TransferCommittedChunk`
  - `TransferCommittedManifest`
- failed upload 结果：
  - `cleanup_candidates`
  - `cleanup_candidate_possible`

### `metadata_transfer_client.h`

当前 Metadata adapter 暴露：

- `CreateWritePlan`
- `CommitObject`
- `HeadObject`
- `GetObjectManifest`

以及对应 summary、leader hint、diagnostic、transport diagnostics。

它只表达 transfer -> MetadataService 适配边界，不实现 discovery，不保存 manifest 权威副本，也不做 upload/download 编排。

### `storage_transfer_client.h`

当前 Storage adapter 暴露：

- `WriteChunk`
- `ReadChunk`

以及：

- `StorageTransferTarget`
- `StorageTransferWriteRequest/Result`
- `StorageTransferReadRequest/Result`
- `StorageTransferClientConfig`

它只负责单 StorageNode 的 chunk RPC 映射，不负责 manifest 选择、对象可见性和整文件编排。

## 当前实现流程

### upload

`object_transfer.cpp` 里的 upload 当前是“两遍 bounded 读取 + metadata authority 提交”流程：

1. 校验请求
2. 第一遍按 bounded chunk 读取源文件
   - 逐 chunk 计算 checksum
   - 增量维护对象级 checksum
   - 记录 `prepared_chunks`
3. 通过 ViewNode `DiscoverMetadata`
4. 调用 `MetadataTransferClient::CreateWritePlan(...)`
   - 当前逻辑边界是：
     - 先创建 pending object
     - 再基于 `prepared_chunks` 和当前 `DiscoverStorage` 结果做 per-chunk placement
     - 再返回完整 `TransferWritePlan`
5. 通过 ViewNode `DiscoverStorage`
6. 第二遍重新按 bounded chunk 读取源文件
7. 对每个 chunk：
   - 构造 `ChunkIdentity`
   - 解析 replica target
   - 调用 `StorageTransferClient::WriteChunk(...)`
   - 收集 durable chunk facts
8. 满足 `minimum_successful_writes` 后，调用 `CommitObject(...)`
9. 只有 MetadataNode 接受并提交后，upload 才会返回 `committed=true`

这里有两个关键边界：

- `CreateWritePlan` / `CommitObject` 都不传真实 payload
- 即使 chunk 已经 durable，未成功 `CommitObject` 前对象也不能被普通读路径视为可见

当前 `TransferWritePlan` 的关键字段包括：

- plan 级：
  - `request_id`
  - `bucket`
  - `object_key`
  - `object_id`
  - `version`
  - `chunk_size_bytes`
  - `total_chunks`
  - `replica_count`
  - `minimum_successful_writes`
  - `placement_epoch`
  - `created_at_unix_ms`
  - `expires_at_unix_ms`
- chunk 级：
  - `identity`
  - `offset`
  - `expected_size`
  - `expected_checksum`
  - `selected_replica_nodes`
  - `candidate_nodes`

其中：

- `selected_replica_nodes` 是 upload 执行 authority
- `candidate_nodes` 当前只保留给兼容路径/诊断；后续 T004 仍需继续清理旧 fallback
- `placement_epoch` 当前来自 ViewNode `DiscoverStorage.observed_at_unix_ms`
- `expires_at_unix_ms` 当前是基于 discovery / CreateWritePlan timeout 推导出的第一阶段客户端有效期边界，不宣称强一致快照承诺

### download

download 当前是 manifest-driven、逐 chunk、临时文件重建流程：

1. 校验请求
2. 通过 ViewNode `DiscoverMetadata`
3. 调用 `MetadataTransferClient::GetObjectManifest(...)`
4. 只接受 COMMITTED manifest
5. 校验 manifest layout
6. 通过 ViewNode `DiscoverStorage`
7. 创建临时输出文件
8. 逐 chunk：
   - 从 manifest 选 replica target
   - 调用 `StorageTransferClient::ReadChunk(...)`
   - 校验 chunk checksum / size
   - 按 offset 写入临时文件
9. 全部 chunk 完成后，校验最终对象 checksum
10. 通过 `rename`/publish 把临时文件切换到最终 `destination_path`

download 不接受：

- PENDING 对象
- ViewNode 推断出来的“看起来可能存在”的对象
- StorageNode 本地 live chunk 列表作为可见性 authority

## 当前 adapter 语义

### Metadata transfer client

`metadata_transfer_client.cpp` 当前采用“现有 MetadataService 最小映射”：

- `CreateWritePlan`
  - 当前先通过现有 `CreateObject` RPC 建立 pending metadata
  - metadata adapter 只负责 object/version/time/policy 等基础 facts
  - 完整 chunk layout / selected replica nodes 由 transfer upload 编排层基于本地 `prepared_chunks` 和当前 storage 观测结果组装
- `CommitObject`
  - 通过现有 `CommitObject` RPC 提交 chunk manifest facts
- `GetObjectManifest`
  - 通过现有 `HeadObject` RPC 读取 COMMITTED `ObjectRecord` 并转换成 transfer manifest

`NOT_LEADER` 边界当前已经实现：

- 同一次 metadata RPC 最多只做一次基于 `leader_hint.leader_address` 的 endpoint 刷新重试
- `leader_hint` 只作为候选地址，不是强一致 leader 事实
- hint 缺失、为空、仍指向当前 endpoint，或重试后仍 `NOT_LEADER` 时，会明确失败，不做无界猜测

### Storage transfer client

`storage_transfer_client.cpp` 当前复用 `StorageNodeClient` 发起单节点 `WriteChunk` / `ReadChunk`，并做：

- endpoint -> channel 复用
- request 校验
- response -> transfer result 映射
- transient failure 有限 retry/backoff

当前 retry/backoff 只对临时失败生效，例如：

- `Timeout`
- `IoError`
- `Overloaded`
- `NodeUnavailable`

不会盲目重试：

- `ChecksumMismatch`
- `Corrupted`
- `InvalidArgument`
- `DiskFull`
- `NotFound`

重试会保留同一 `request_id` / chunk identity，且受 `max_transient_*_retries`、`initial_backoff_ms`、`max_backoff_ms` 和总 deadline 约束。

## cleanup candidate 边界

upload 失败路径当前会发出 failed upload cleanup candidate：

- 当 chunk 已 durable，但 `minimum_successful_writes` 未满足
- 当 `CommitObject` 失败
- 当第二遍上传过程中出现 IO / checksum / 顺序冲突

当前结果里会保留：

- `committed_chunks`
  - 已 durable、原本准备提交给 metadata 的 chunk facts
- `cleanup_candidates`
  - 交给 cleanup hook / 后续维护流程的候选
- `cleanup_candidate_possible`
  - 包含 retryable / uncertain placement 的保守风险标记

这里的核心边界是：

- cleanup candidate 不是 COMMITTED 对象
- transfer 不会直接删除 live chunk
- transfer 不会因为 cleanup 需要而绕过 MetadataNode authority 或 StorageNode contract

## bounded memory / bounded concurrency

当前实现明确保持 bounded 文件与 bounded payload 路径：

- 文件读取通过 `TransferChunkReader`
- 对象级 checksum 通过 `TransferChecksumState` 增量维护
- upload 第二遍 reread 仍按 chunk 方式进行
- download 逐 chunk 读取并写入临时文件

T083 当前又把 session 级并发预算显式收紧成单 chunk in-flight：

- `requested_concurrency` 会被解析成 `SessionConcurrencyBudget`
- 当前 `effective_concurrency` 明确限制为 1
- `max_inflight_chunks` / `max_buffered_chunks` / `max_task_slots` 都是显式有界值
- 更大的 CLI `concurrency` 只会进入诊断，不会演化成无界线程、无界队列或整文件常驻内存

这意味着当前实现是“有界且可诊断”的，不是“真正多 chunk 并发 pipeline”。

## 关键非职责边界

本模块不负责：

- 决定对象是否 `COMMITTED` 可见
- 保存 manifest 权威副本
- 修改 Raft membership、quorum、leader election、commit 规则
- 直接读写 StorageNode 本地 chunk 文件、索引、publish 状态
- 让真实 payload 进入：
  - Raft log
  - Raft snapshot
  - metadata snapshot
  - task report
- app 启动循环

尤其要注意：

- ViewNode discovery 结果只是候选 endpoint，不是对象可见性 authority
- StorageNode 本地可读不代表对象对普通下载可见
- `committed_chunks` / `cleanup_candidates` 都不能被上层误解成对象已经提交

## 失败路径和诊断边界

当前传输侧会显式返回：

- discovery failure
- metadata rejection / `NOT_LEADER`
- storage write/read failure
- checksum mismatch
- commit failure
- partial progress
- retryable / non-retryable 边界

diagnostic 当前会尽量带上：

- `request_id`
- `node_id`
- `endpoint`
- `chunk_id`
- `chunk_index`
- `offset`
- `retryable`

目标是让“失败发生在哪一层、哪一个 chunk、是否可重试、是否需要 cleanup follow-up”都可追踪。

## 与其他模块的交互

- `modules/view/`
  - 只提供 MetadataNode / StorageNode discovery 与 cluster observation
- `modules/raft/service` / MetadataNode
  - 决定 pending/committed 和 manifest 权威事实
- `modules/store/chunk` / StorageNode
  - 决定真实 chunk 的 durable publish、checksum、read、recovery
- `apps/storage_client.cpp`
  - 只负责 CLI 参数与结果输出，不重写 transfer 业务逻辑

## 当前状态和后续边界

- 已实现：
  - upload/download 编排主路径
  - manifest-driven download
  - metadata `NOT_LEADER` 有界重试
  - Storage transient retry/backoff
  - failed upload cleanup candidate emission
  - session 级 bounded concurrency budget
- 仍是后续能力或未收口项：
  - 真正的多 chunk 并发 pipeline
  - 多副本读降级切换
  - resumable transfer
  - streaming RPC
  - 真正启用的 100-op round-trip 并发验收

后续扩展这些能力时，必须继续保持“metadata authority 在 MetadataNode，payload 只走 StorageNode，transfer 只做 bounded orchestration”这三条硬边界。
