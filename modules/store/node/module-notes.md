# store/node 说明

## 模块职责

`modules/store/node` 承载 StorageNode data-plane 的 RPC 适配层，以及供后续 heartbeat / placement / read-side 消费的 in-memory `StorageNodeRegistry`。

当前只负责：

- `StorageNodeRegistry` 的 in-memory register / heartbeat / partial report merge / lookup / list / snapshot
- `StorageNodeService::WriteChunk` 的 gRPC `WriteChunk` 入口
- `StorageNodeService::ReadChunk` 的 gRPC `ReadChunk` 入口
- `StorageNodeService::DeleteChunk` 的 gRPC `DeleteChunk` 入口
- `StorageNodeService::ScrubChunk` 的 gRPC `ScrubChunk` 入口
- `StorageNodeService::BatchDeleteChunks` 的 gRPC `BatchDeleteChunks` 入口
- `StorageNodeService::RegisterStorageNode` 的 gRPC `RegisterStorageNode` 入口
- `StorageNodeService::UpdateStorageNodeHeartbeat` 的 gRPC `UpdateStorageNodeHeartbeat` 入口
- `StorageNodeService::ReportHealth` 的 gRPC `ReportHealth` 入口
- `StorageNodeService::ReportCapacity` 的 gRPC `ReportCapacity` 入口
- `StorageNodeService::ReportLoad` 的 gRPC `ReportLoad` 入口
- `StorageNodeClient::WriteChunk` 的本地请求到 gRPC `WriteChunk` 调用
- `StorageNodeClient::ReadChunk` 的本地请求到 gRPC `ReadChunk` 调用
- `StorageNodeClient::DeleteChunk` 的本地请求到 gRPC `DeleteChunk` 调用
- `StorageNodeClient::ScrubChunk` 的本地请求到 gRPC `ScrubChunk` 调用
- `StorageNodeClient::BatchDeleteChunks` 的本地请求到 gRPC `BatchDeleteChunks` 调用
- `StorageNodeClient::RegisterStorageNode` 的本地请求到 gRPC `RegisterStorageNode` 调用
- `StorageNodeClient::UpdateStorageNodeHeartbeat` 的本地请求到 gRPC `UpdateStorageNodeHeartbeat` 调用
- `StorageNodeClient::ReportHealth` / `ReportCapacity` / `ReportLoad` 的本地请求到对应 gRPC report 调用
- committed manifest 读路径可复用的最小 request builder、错误分类和 replica fallback helper
- proto `WriteChunkRequest` 到 `ChunkStore::WriteChunk` 的字段转换
- proto `ReadChunkRequest` 到 `ChunkStore::ReadChunk` 的字段转换
- proto `ScrubChunkRequest` 到 `ChunkStore::StatChunk` 的字段转换
- proto `DeleteChunkRequest` / `BatchDeleteChunkRequest` 到 `ChunkStore::DeleteChunk` 的字段转换
- `ChunkStore` 结果到 `storage_node.proto` 响应的状态码、checksum、state、durable、already_exists 映射
- `ChunkStore` 的 `ReadChunkResponse` 到 `storage_node.proto::ReadChunkResponse` 的状态码、checksum、state、payload、offset、complete/full_read 映射
- `ChunkStore` 的 `StatChunk` 结果到 `storage_node.proto::ScrubChunkResponse` 的状态码、checksum、size、state、corrupted/quarantine/missing facts 映射
- `ChunkStore` 的 `DeleteChunkResponse` 到 `storage_node.proto::DeleteChunkResponse` / `BatchDeleteChunkResult` 的状态码、checksum、state、idempotent、retryable 映射
- `storage_node.proto`（`package storage`）`WriteChunkResponse` 和 gRPC status 到本地 `storedemo::WriteChunkResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`ReadChunkResponse` 和 gRPC status 到本地 `storedemo::ReadChunkResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`ScrubChunkResponse` 和 gRPC status 到本地 `storedemo::StorageNodeClientScrubChunkResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`DeleteChunkResponse` 和 gRPC status 到本地 `storedemo::StorageNodeClientDeleteChunkResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`BatchDeleteChunksResponse` 和 gRPC status 到本地 `storedemo::StorageNodeClientBatchDeleteChunksResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`RegisterStorageNodeResponse` 和 gRPC status 到本地 `storedemo::StorageNodeClientRegisterStorageNodeResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`StorageNodeFactUpdateResponse` 和 gRPC status 到本地 `storedemo::StorageNodeClientFactUpdateResponse` / `StorageNodeStatusCode` 的映射

当前不负责：

- `StatChunk` / `ListChunks`
- metadata `CreateObject` / `CommitObject` / `AbortObject`
- `RaftNode::ProposeMetadata()`
- upload coordinator
- gRPC server 启动与进程生命周期管理

## 主要文件

- `storage_node_service.h`：service 类声明
- `storage_node_service.cpp`：`WriteChunk` / `ReadChunk` / `DeleteChunk` / `BatchDeleteChunks` / `RegisterStorageNode` / `UpdateStorageNodeHeartbeat` / `ReportHealth` / `ReportCapacity` / `ReportLoad` 适配实现和 proto/store/registry 映射 helper
- `storage_node_client.h`：client 类声明，以及 write/read/delete/batch-delete/register/heartbeat/report 选项与本地请求/响应结构
- `storage_node_client.cpp`：同步 `WriteChunk` / `ReadChunk` / `DeleteChunk` / `BatchDeleteChunks` / `RegisterStorageNode` / `UpdateStorageNodeHeartbeat` / `ReportHealth` / `ReportCapacity` / `ReportLoad` 调用、deadline 设置和响应映射
- `storage_node_registry.h`：registry 请求/结果结构、liveness 枚举和 `StorageNodeRegistry` 接口
- `storage_node_registry.cpp`：registry 注册、sequence/stale 保护、partial merge、liveness 与稳定快照实现

## T062 StorageNodeRegistry 边界

- `StorageNodeRegistry` 只保存 StorageNode data-plane facts：capacity、health、disk pressure、io_error_count、load、failure domain、last_sequence、last_seen。
- register / heartbeat / report 只维护内存态 registry，不写 metadata，不写 Raft，不保存 object payload。
- liveness 由 registry 侧 `last_seen + timeout` 推导，不接受节点自报 committed/deleted/object visibility 决策。
- 当前 registry 已接入 gRPC service 和 gRPC client 适配层；`PlacementManager` 和 read replica selection 接线分别留给 T065-T066。

## storage_node_registry.cpp 关键 helper

### `InitializeRegisteredRecord(Record*, std::string, const StorageNodeRegistryFacts&, std::uint64_t)`

- 责任：初始化新注册节点的 endpoint、初始 facts 和 `last_seen`
- 输入：注册 endpoint、注册 facts、`observed_at_unix_ms`
- 输出：填充好的内存记录
- 边界：只用于新注册路径；`last_sequence` 保持 0，等待后续 heartbeat/report 推进

### `EvaluateSequenceDecision(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t)`

- 责任：统一判断 sequenced update 是 apply、idempotent 还是 stale
- 输入：当前 `last_sequence/last_seen` 与传入 `sequence/observed_at`
- 输出：`kApply`、`kIdempotent` 或 `kStale`
- 边界：sequence 变大但 `observed_at` 回退时仍按 stale 处理，避免旧 heartbeat/report 覆盖新 facts

### `MergeHealthFacts(...)` / `MergeCapacityFacts(...)` / `MergeLoadFacts(...)`

- 责任：把局部 health/capacity/load report 合并进已有节点 facts
- 输入：目标记录、局部 report、sequence、observed time
- 输出：更新后的记录和 `last_sequence/last_seen`
- 边界：只覆盖对应分组，不清掉未上报的其它 facts

### `DetermineLiveness(std::uint64_t, std::uint64_t, const StorageNodeRegistryConfig&)`

- 责任：根据 `last_seen` 和配置的 stale/dead timeout 推导节点 liveness
- 输入：`last_seen_unix_ms`、当前时间、registry config
- 输出：`kLive`、`kStale` 或 `kDead`
- 边界：当前时间早于或等于 `last_seen` 时按 `kLive` 处理，不在 registry 内引入额外时钟校正

### `AppendSortedSnapshots(const Records&, std::uint64_t, const StorageNodeRegistryConfig&, std::vector<StorageNodeRegistryNodeSnapshot>*)`

- 责任：为 `ListNodes()` / `Snapshot()` 构造稳定排序的节点快照
- 输入：registry 当前记录、查询时间、config
- 输出：按 `node_id` 稳定排序的 snapshot 列表
- 边界：排序依赖 `records_` 的 `std::map<StorageNodeId, ...>` 键序，不额外发明热点或健康排序

### `ValidateNodeIdentity(...)` / `ValidateObservedAt(...)` / `ValidateSequence(...)` / `ValidateCapacityFacts(...)`

- 责任：集中校验 node identity、时间戳、sequence 和容量 facts
- 输入：请求字段与错误输出指针
- 输出：`StorageNodeStatusCode`
- 边界：当前只对 node_id、endpoint、observed_at、sequence、capacity 做显式校验；load/health 的业务打分仍留给后续 placement/read-side 消费

## T031/T032 固定边界

- `StorageNodeService` 必须通过构造函数注入 `ChunkStore`，不在 service 内部创建全局 `LocalDiskChunkStore`
- `StorageNodeClient` 必须通过构造函数注入 generated stub 或 gRPC channel，不直接依赖 metadata / Raft 模块
- `WriteChunk` 成功只表示 chunk 已经按当前 store contract durable publish，不表示 metadata object 已 committed
- `ReadChunk` 成功只表示当前 chunk data-plane 读取成功，不表示 metadata object 已 committed，也不决定 object 可见性
- `DeleteChunk` / `BatchDeleteChunks` 成功只表示 chunk data-plane 删除 contract 已执行，不表示 metadata object 已 deleted，也不决定 object 可见性
- service 只调用 `ChunkStore::WriteChunk()`，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- service 的 `ReadChunk` 只调用 `ChunkStore::ReadChunk()`，不调用 metadata service / `MetadataStateMachine` / `RaftNode::ProposeMetadata()`
- service 的 `DeleteChunk` / `BatchDeleteChunks` 只调用 `ChunkStore::DeleteChunk()`，不调用 metadata service / `MetadataStateMachine` / `RaftNode::ProposeMetadata()`
- client 只调用 `storage_node.proto` 的 `WriteChunk` RPC，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- client 的 `ReadChunk` 只调用 `storage_node.proto` 的 `ReadChunk` RPC，不调用 metadata service / `MetadataStateMachine` / `RaftNode::ProposeMetadata()`
- client 的 `DeleteChunk` / `BatchDeleteChunks` 只调用 `storage_node.proto` 的对应删除 RPC，不调用 metadata service / `MetadataStateMachine` / `RaftNode::ProposeMetadata()`
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload` 都会转换到 `ChunkStore::WriteChunk` 请求
- `ReadChunk` 当前会把 `request_id`、`chunk_id`、`expected_checksum`、`verify_checksum` 转到 `ChunkStore::ReadChunk`；如果 `chunk_id` 为空，则尝试用 `object_id + version + chunk_index` 派生 chunk id；如果 `length > 0`，则把 `offset + length` 转成 `ChunkReadRange`
- `DeleteChunk` 当前会把 `request_id`、`chunk_id`、`reason`、`metadata_boundary`、`expected_checksum` 转到 `ChunkStore::DeleteChunk`；如果 `chunk_id` 为空，则尝试用 `object_id + version + chunk_index` 派生 chunk id；如果显式 `chunk_id` 与 object identity 同时出现但不一致，则在 service 层返回显式参数错误，避免误删 live chunk
- `BatchDeleteChunks` 当前按请求顺序逐项构造 `ChunkStore::DeleteChunk` 请求，并把 top-level `request_id` 扩展成 `/item/<index>` 形式的逐项 request id；逐项结果和聚合计数必须保持一致
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload`、`timeout_ms`、`best_effort_cancel`、`durability` 都会转换到 `storage_node.proto::WriteChunkRequest`
- client 的 `ReadChunk` 会把本地 `request_id`、`chunk_id`、`range(offset/length)`、`expected_checksum`、`verify_checksum`、`timeout_ms`、`best_effort_cancel` 转换到 `storage_node.proto::ReadChunkRequest`；本地请求没有 `object_id/version/chunk_index` 时，不伪造对象可见性语义
- client 的 `DeleteChunk` 会把本地 `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`expected_checksum`、`reason`、`metadata_boundary`、`timeout_ms`、`best_effort_cancel` 转换到 `storage_node.proto::DeleteChunkRequest`；本地请求保留 chunk data-plane 删除身份语义，不发明 object deleted 可见性
- client 的 `BatchDeleteChunks` 会把本地 top-level `request_id`、逐项 `chunk_id/object_id/version/chunk_index/expected_checksum/reason/metadata_boundary`、`timeout_ms`、`best_effort_cancel` 转换到 `storage_node.proto::BatchDeleteChunksRequest`；逐项结果、聚合计数和 partial failure 语义以 proto/service 返回事实为准
- client 的 `RegisterStorageNode` 会把本地 `request_id`、`node_id`、`endpoint`、`observed_at_unix_ms`、`facts` 转换到 `storage_node.proto::RegisterStorageNodeRequest`；当前 proto 没有该 RPC 的 `timeout_ms` / `best_effort_cancel` 字段，因此这两个控制面选项只作用于 gRPC `ClientContext` deadline / 本地边界说明
- client 的 `UpdateStorageNodeHeartbeat` 会把本地 `request_id`、`node_id`、`endpoint`、`sequence`、`observed_at_unix_ms`、`facts` 转换到 `storage_node.proto::UpdateStorageNodeHeartbeatRequest`；same-sequence / stale 语义以 service/registry 返回事实为准
- client 的 `ReportHealth` / `ReportCapacity` / `ReportLoad` 会把本地 identity、`sequence`、`observed_at_unix_ms` 和局部 facts 转到对应 proto request；partial merge 规则仍由 service + registry 决定
- `storage_node.proto::ReadChunkRequest` 中的 `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，不会在 service/store 内伪装成已经具备运行中取消传播
- proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 都以 `ChunkStore` 返回事实为准
- `ReadChunkResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`chunk_id`、`payload`、`size`、`checksum`、`state`、`offset`、`complete`、`full_read` 都以 `ChunkStore::ReadChunk` 返回事实和当前 request range 语义为准
- `DeleteChunkResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`chunk_id`、`size`、`checksum`、`state`、`deleted`、`already_missing`、`already_deleted`、`retryable` 都以 `ChunkStore::DeleteChunk` 返回事实和当前 request identity/checksum 边界为准
- `BatchDeleteChunksResponse` 的 `results`、`success_count`、`idempotent_count`、`retryable_failure_count`、`non_retryable_failure_count`、`partial_failure` 都以逐项 `ChunkStore::DeleteChunk` 结果聚合，不在 node 层发明 metadata 可见性语义
- client 会把 proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 转回本地 `storedemo::WriteChunkResponse`
- client 会把 proto `ReadChunkResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`chunk_id`、`payload`、`size`、`checksum`、`state`、`offset` 转回本地 `storedemo::ReadChunkResponse`；如果 proto 成功但 `complete/full_read` 语义与本地请求不一致，会显式映射成 `IO_ERROR`，不做 silent success
- client 会把 proto `DeleteChunkResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`chunk_id`、`size`、`checksum`、`state`、`deleted`、`already_missing`、`already_deleted`、`retryable` 转回本地 `storedemo::StorageNodeClientDeleteChunkResponse`；如果 proto identity/checksum/state 非法，会显式映射成 `IO_ERROR`
- client 会把 proto `BatchDeleteChunksResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`results`、`success_count`、`idempotent_count`、`retryable_failure_count`、`non_retryable_failure_count`、`partial_failure` 转回本地 `storedemo::StorageNodeClientBatchDeleteChunksResponse`；如果逐项结果数量或聚合计数与逐项事实不一致，会显式映射成 `IO_ERROR`
- client 会把 proto `RegisterStorageNodeResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`created`、`idempotent`、`snapshot` 转回本地 `storedemo::StorageNodeClientRegisterStorageNodeResponse`；如果 snapshot/liveness/facts 非法，会显式映射成 `IO_ERROR`
- client 会把 proto `StorageNodeFactUpdateResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`accepted_sequence`、`applied`、`idempotent`、`stale_ignored`、`snapshot` 转回本地 `storedemo::StorageNodeClientFactUpdateResponse`；如果 snapshot 非法，会显式映射成 `IO_ERROR`

## timeout / cancellation / durability 边界

- `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，T031 不会把它们包装成“已经具备运行中取消传播”的既成事实
- 原因是当前 `ChunkStore::WriteChunk` / `ChunkStore::ReadChunk` 接口都还没有 `StorageTaskContext` 入参，runtime 侧也尚未实现真实运行中 deadline/cancellation 传播
- T032 会在 client 侧把 `timeout_ms` 同时写入 proto request，并映射成 gRPC `ClientContext` deadline；当前 deadline 只约束 RPC 生命周期，不意味着 service/store 已具备运行中中断能力
- `best_effort_cancel` 当前只会作为 proto 字段透传，不会在 client 侧伪装为完整的 end-to-end cancellation propagation
- `WriteChunkDurability` 目前只接受 `UNSPECIFIED` 或 `PUBLISH`；当前 `LocalDiskChunkStore::WriteChunk` 的成功语义就是完成 durable publish
- `ReadChunk` 当前如果收到 `length > 0` 的 range request，会原样传给 `ChunkStore::ReadChunk`；是否支持 range 由底层 store 决定。当前 `LocalDiskChunkStore` 会返回显式 `UNSUPPORTED`，service 不做 silent partial success
- `LocalDiskChunkStore::ReadChunk` 当前在 checksum mismatch / corrupted 场景只返回明确错误，不会自动回写 `CORRUPTED` / `QUARANTINED`；service 适配层保持这个边界，不在 node 层强行发明状态回写
- `DeleteChunkRequest` / `BatchDeleteChunksRequest` 中的 `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，T052 不会把它们包装成已经具备运行中取消传播或后台删除调度
- `BatchDeleteChunks` 当前逐项串行调用 `ChunkStore::DeleteChunk()`；`retryable/non-retryable` 分类直接复用 `storedemo::IsRetriableStatus()`，不在 service 层发明新的状态体系
- `StorageNodeClient` 当前支持有限自动重试，但只会重试 retryable 状态：`TIMEOUT`、`IO_ERROR`、`OVERLOADED`、`NODE_UNAVAILABLE`；`CONFLICT`、`CHECKSUM_MISMATCH`、`INVALID_ARGUMENT`、`CANCELLED` 等非 retryable 结果不会重试
- client 的 `timeout_ms` 当前作为整次 `WriteChunk` 调用的绝对 deadline 预算；每次重试共用同一个 deadline，不会无限延长总等待时间
- `StorageNodeClient::ReadChunk` 自身仍保持单副本、单次 RPC；T045 额外提供独立 helper 供 committed manifest 读路径按 selector 结果做副本 fallback，但不把副本选择硬编码进单节点 client
- `StorageNodeClient::DeleteChunk` / `BatchDeleteChunks` 当前也保持单次 RPC，不自动重试；retryable/non-retryable 分类只用于把后续重试决策显式返回给调用方
- client 的 `DeleteChunk` / `BatchDeleteChunks` 会把 `timeout_ms` 同时写入 proto request，并映射成 gRPC `ClientContext` deadline；`best_effort_cancel` 仍只做字段透传，不伪装成已经具备 service/store 运行中取消传播
- client 的 `RegisterStorageNode` / `UpdateStorageNodeHeartbeat` / `ReportHealth` / `ReportCapacity` / `ReportLoad` 也会把本地 `timeout_ms` 映射成 gRPC `ClientContext` deadline；但由于 T061 既有 proto 没有这些 RPC 的 `timeout_ms` / `best_effort_cancel` 字段，当前不会伪装成 service/registry 已观察到 end-to-end cancellation hint
- control-plane client 当前不自动重试 `RegisterStorageNode` / heartbeat / report；调用方需要根据 `TIMEOUT`、`NODE_UNAVAILABLE`、`CANCELLED` 等显式状态自行决定是否重试

## storage_node_client.cpp 关键 helper

### `FillProtoRegisterRequest(...)` / `FillProtoHeartbeatRequest(...)`

- 责任：把本地 register / heartbeat 请求转换成 proto request
- 输入：本地 client request、registry rpc 选项、proto 输出对象
- 输出：填充好的 proto register / heartbeat 请求
- 边界：当前只复用 T061 既有 schema；register/heartbeat proto 没有 `best_effort_cancel` 字段，因此取消 hint 不会伪装成已透传到 service

### `FillProtoHealthReportRequest(...)` / `FillProtoCapacityReportRequest(...)` / `FillProtoLoadReportRequest(...)`

- 责任：把本地 partial report 请求转换成 proto health/capacity/load report
- 输入：本地 client request、registry rpc 选项、proto 输出对象
- 输出：填充好的 proto report 请求
- 边界：只负责字段映射，不在 client 侧决定 partial merge 语义

### `TranslateProtoRegisterResponse(...)` / `TranslateProtoFactUpdateResponse(...)`

- 责任：把 proto register / fact-update 响应转换成本地 client response
- 输入：proto response、操作名
- 输出：本地 `StorageNodeClientRegisterStorageNodeResponse` / `StorageNodeClientFactUpdateResponse`
- 边界：非法 snapshot/liveness/facts 一律显式映射成 `IO_ERROR`，不 silent success

### `MakeGrpcRegisterFailureResponse(...)` / `MakeGrpcFactUpdateFailureResponse(...)`

- 责任：统一把非 OK gRPC status 映射到本地 control-plane response
- 输入：gRPC `Status`
- 输出：本地 client response
- 边界：`DEADLINE_EXCEEDED -> TIMEOUT`、`CANCELLED -> CANCELLED`、`UNAVAILABLE -> NODE_UNAVAILABLE`，其它未知/内部错误落到明确错误码，不返回成功

### `StorageNodeClient::RegisterStorageNode(...)`

- 责任：同步发起 `RegisterStorageNode` RPC，并把 proto/gRPC 结果映射回本地 register 响应
- 输入：本地 register request、registry rpc 选项
- 输出：本地 `StorageNodeClientRegisterStorageNodeResponse`
- 边界：当前只做一次 RPC，不自动重试；`timeout_ms` 只约束 gRPC deadline

### `StorageNodeClient::UpdateStorageNodeHeartbeat(...)`

- 责任：同步发起 `UpdateStorageNodeHeartbeat` RPC，并把 proto/gRPC 结果映射回本地 fact-update 响应
- 输入：本地 heartbeat request、registry rpc 选项
- 输出：本地 `StorageNodeClientFactUpdateResponse`
- 边界：same-sequence / stale / accepted-sequence 语义完全跟随 service + registry 返回事实

### `StorageNodeClient::ReportHealth(...)` / `StorageNodeClient::ReportCapacity(...)` / `StorageNodeClient::ReportLoad(...)`

- 责任：同步发起 control-plane partial report RPC，并把 proto/gRPC 结果映射回本地 fact-update 响应
- 输入：本地 health/capacity/load report、registry rpc 选项
- 输出：本地 `StorageNodeClientFactUpdateResponse`
- 边界：client 不决定 partial merge、liveness 或 placement/read-side 消费策略，只表达 service + registry 的既有 contract

## storage_node_service.cpp 关键 helper

### `ToProtoStatusCode(StorageNodeStatusCode)`

- 责任：统一把本地 `StorageNodeStatusCode` 映射成 `storage::StorageNodeStatusCode`
- 输入：本地状态码
- 输出：proto 状态码
- 边界：service 侧 write/read/delete/heartbeat/report/register 都复用同一状态体系，不额外发明新错误码

### `TranslateRegisterRequest(const storage::RegisterStorageNodeRequest&, RegisterStorageNodeRequest*)`

- 责任：把 proto `RegisterStorageNodeRequest` 转成 `StorageNodeRegistry` 的本地注册请求
- 输入：proto register request、本地 registry request 输出指针
- 输出：成功时填充 `node_id`、`endpoint`、`observed_at_unix_ms` 和初始 facts
- 边界：`request_id` 只保留在 proto summary，不进入 registry；facts 字段转换失败时返回显式 `INVALID_ARGUMENT`

### `TranslateHeartbeatRequest(const storage::UpdateStorageNodeHeartbeatRequest&, UpdateStorageNodeHeartbeatRequest*)`

- 责任：把 proto heartbeat request 转成 `StorageNodeRegistry` 的全量 heartbeat 请求
- 输入：proto heartbeat request、本地 registry request 输出指针
- 输出：成功时填充 `node_id`、`endpoint`、`sequence`、`observed_at_unix_ms` 和全量 facts
- 边界：要求请求内存在 heartbeat message；当前不在 service 层发明独立时间源或 sequence

### `TranslateHealthReportRequest(...)` / `TranslateCapacityReportRequest(...)` / `TranslateLoadReportRequest(...)`

- 责任：把 proto 局部 report 请求转成 `StorageNodeRegistry` 的 partial report 请求
- 输入：proto health/capacity/load request、本地 registry request 输出指针
- 输出：成功时填充 identity、`sequence`、`observed_at_unix_ms` 和对应 facts 分组
- 边界：只转换各自负责的 facts；partial merge 规则仍由 `StorageNodeRegistry` 决定

### `FillRegisterResponse(const storage::RegisterStorageNodeRequest&, const RegisterStorageNodeResult&, storage::RegisterStorageNodeResponse*)`

- 责任：把 registry register 结果映射回 proto `RegisterStorageNodeResponse`
- 输入：proto register request、registry register result、proto response 输出指针
- 输出：`summary`、`created`、`idempotent`、`snapshot`
- 边界：summary 的 `request_id` 来自 proto request；`chunk_id` 保持空；snapshot 只反映 registry 事实

### `FillFactUpdateResponse(std::string_view, std::string_view, const StorageNodeRegistryUpdateResult&, storage::StorageNodeFactUpdateResponse*)`

- 责任：把 registry heartbeat/report 结果映射回 proto `StorageNodeFactUpdateResponse`
- 输入：proto request_id / node_id、registry update result、proto response 输出指针
- 输出：`summary`、`accepted_sequence`、`applied`、`idempotent`、`stale_ignored`、`snapshot`
- 边界：service 不重新解释 stale/idempotent 语义，直接复用 registry 结果

### `StorageNodeService::RegisterStorageNode(...)`

- 责任：承接 gRPC `RegisterStorageNode` RPC，请求转换后调用注入的 `StorageNodeRegistry::RegisterStorageNode()`
- 输入：gRPC context、proto register request
- 输出：proto `RegisterStorageNodeResponse`
- 边界：只做字段和状态映射；不调用 metadata / Raft；不保存 payload

### `StorageNodeService::UpdateStorageNodeHeartbeat(...)`

- 责任：承接 gRPC `UpdateStorageNodeHeartbeat` RPC，请求转换后调用注入的 `StorageNodeRegistry::UpdateStorageNodeHeartbeat()`
- 输入：gRPC context、proto heartbeat request
- 输出：proto `StorageNodeFactUpdateResponse`
- 边界：same-sequence / stale / accepted-sequence 语义完全跟随 registry，不在 service 层重新发明判定

### `StorageNodeService::ReportHealth(...)` / `StorageNodeService::ReportCapacity(...)` / `StorageNodeService::ReportLoad(...)`

- 责任：承接 gRPC partial report RPC，请求转换后调用注入的 `StorageNodeRegistry` partial merge 接口
- 输入：gRPC context、对应 proto report request
- 输出：proto `StorageNodeFactUpdateResponse`
- 边界：只表达 data-plane facts update，不接 metadata commit、不接 PlacementManager、不接 read replica selection

### `ResolveResponseChunkId(const storage::ReadChunkRequest&, const ReadChunkResponse&)`

- 责任：为 `ReadChunkResponse.summary.chunk_id` 和顶层 `chunk_id` 生成稳定返回值
- 输入：proto `ReadChunkRequest`、store `ReadChunkResponse`
- 输出：优先使用 store metadata 的 chunk id；否则回退到 request.chunk_id；仍为空时尝试由 `object_id + version + chunk_index` 派生
- 边界：派生失败时返回空字符串，不在 helper 内制造新的逻辑错误状态

### `FillSummary(const storage::ReadChunkRequest&, const ReadChunkResponse&, const std::string&, storage::StorageNodeResponseSummary*)`

- 责任：把 store 读结果中的状态码、错误消息、request_id、node_id、chunk_id、retry_after_ms 写回 proto summary
- 输入：proto read request、store read response、service 配置的 node_id
- 输出：完整 `StorageNodeResponseSummary`
- 边界：`node_id` 优先使用 store metadata 的 node_id；为空时回退到 service 构造时的 node_id

### `MakeReadValidationError(StorageNodeStatusCode, std::string)`

- 责任：构造 service 本地请求校验失败时使用的 `ReadChunkResponse`
- 输入：目标状态码、错误详情
- 输出：带 `status` / `error_detail` 的 store-style response
- 边界：只用于 adapter 层参数校验，不触发底层 `ChunkStore`

### `ResolveDeleteRequestChunkId(std::string_view, std::string_view, std::uint64_t, std::uint32_t, ChunkId*, std::string*)`

- 责任：统一收口 `DeleteChunk` / `BatchDeleteChunkRequest` 的 chunk identity 解析
- 输入：显式 `chunk_id`、可选 `object_id/version/chunk_index`
- 输出：稳定的本地 `ChunkId`
- 边界：`chunk_id` 为空时允许由 object identity 派生；两者同时出现但不一致时返回显式参数错误，避免误删

### `MakeDeleteValidationError(StorageNodeStatusCode, std::string)`

- 责任：构造 service 本地请求校验失败时使用的 `DeleteChunkResponse`
- 输入：目标状态码、错误详情
- 输出：带 `status` / `error_detail` 的 store-style delete response
- 边界：只用于 adapter 层参数校验，不触发底层 `ChunkStore`

### `TranslateDeleteRequest(const storage::DeleteChunkRequest&, DeleteChunkRequest*)`

- 责任：把 proto `DeleteChunkRequest` 转成 `ChunkStore::DeleteChunk` 所需本地请求
- 输入：proto delete request、store request 输出指针
- 输出：成功时填充 `request_id`、`chunk_id`、`reason`、`metadata_boundary`、`expected_checksum`
- 边界：优先使用显式 `chunk_id`；必要时由 object identity 派生；当前不会传播 `timeout_ms` / `best_effort_cancel`

### `TranslateBatchDeleteItemRequest(const storage::BatchDeleteChunksRequest&, const storage::BatchDeleteChunkRequest&, std::size_t, DeleteChunkRequest*)`

- 责任：把 batch 中的单个 proto item 转成一次 `ChunkStore::DeleteChunk` 请求
- 输入：top-level batch request、单个 batch item、item index、store request 输出指针
- 输出：带 `/item/<index>` request id 的本地 delete request
- 边界：逐项独立校验；单个 item 校验失败不会阻止其它 item 继续处理

### `TranslateReadRequest(const storage::ReadChunkRequest&, ReadChunkRequest*)`

- 责任：把 proto `ReadChunkRequest` 转成 `ChunkStore::ReadChunk` 所需本地请求
- 输入：proto request、store request 输出指针
- 输出：成功时填充 `request_id`、`chunk_id`、`range`、`expected_checksum`、`verify_checksum`
- 边界：`chunk_id` 为空时尝试由 `object_id + version + chunk_index` 派生；若 `length > 0` 则视为 range request；当前不会传播 `timeout_ms` / `best_effort_cancel`

### `FillReadResponse(const storage::ReadChunkRequest&, const ReadChunkResponse&, const std::string&, storage::ReadChunkResponse*)`

- 责任：把 `ChunkStore::ReadChunk` 的结果映射为 proto `ReadChunkResponse`
- 输入：proto read request、store read response、service 配置的 node_id
- 输出：`summary`、`chunk_id`、`payload`、`size`、`checksum`、`state`、`offset`、`complete`、`full_read`
- 边界：`checksum` 优先使用 `actual_checksum`；`complete/full_read` 只在 store 成功且当前 request 不带 range 时返回 full read

### `StorageNodeService::ReadChunk(...)`

- 责任：承接 gRPC `ReadChunk` RPC，请求校验后调用注入的 `ChunkStore::ReadChunk()`，再把结果映射回 proto 响应
- 输入：gRPC context、proto `ReadChunkRequest`
- 输出：proto `ReadChunkResponse`
- 边界：只做字段和状态映射；不调用 metadata / Raft；不决定 object committed 可见性；当前只明确接收 `timeout_ms` / `best_effort_cancel` contract，不承诺运行中取消传播

### `FillDeleteResponse(const storage::DeleteChunkRequest&, const DeleteChunkResponse&, const std::string&, storage::DeleteChunkResponse*)`

- 责任：把 `ChunkStore::DeleteChunk` 的结果映射为 proto `DeleteChunkResponse`
- 输入：proto delete request、store delete response、service 配置的 node_id
- 输出：`summary`、`chunk_id`、`size`、`checksum`、`state`、`deleted`、`already_missing`、`already_deleted`、`retryable`
- 边界：`already_deleted` 由 `already_missing + state == DELETED` 推导；`retryable` 直接复用本地 retryable 状态分类

### `FillBatchDeleteResult(const storage::BatchDeleteChunksRequest&, const storage::BatchDeleteChunkRequest&, std::size_t, const DeleteChunkResponse&, const std::string&, storage::BatchDeleteChunkResult*)`

- 责任：把单个 `ChunkStore::DeleteChunk` 结果映射成一个 proto `BatchDeleteChunkResult`
- 输入：top-level batch request、单个 batch item、item index、store delete response、service 配置的 node_id
- 输出：逐项 `summary`、`chunk_id`、`size`、`checksum`、`state`、`deleted`、idempotent/retryable 标记
- 边界：复用单删响应映射，不发明单独的 batch-only 状态体系

### `FillBatchSummary(const storage::BatchDeleteChunksRequest&, const std::string&, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t, storage::StorageNodeResponseSummary*)`

- 责任：为 `BatchDeleteChunksResponse.summary` 聚合 top-level 事实
- 输入：batch request、service node_id、success/idempotent/retryable/non-retryable 计数、聚合 retry_after
- 输出：top-level summary
- 边界：summary 只表达 batch RPC 已执行和是否存在 item failures；逐项成功/失败仍以 `results` 为准

### `StorageNodeService::DeleteChunk(...)`

- 责任：承接 gRPC `DeleteChunk` RPC，请求校验后调用注入的 `ChunkStore::DeleteChunk()`，再把结果映射回 proto 响应
- 输入：gRPC context、proto `DeleteChunkRequest`
- 输出：proto `DeleteChunkResponse`
- 边界：只做字段和状态映射；不调用 metadata / Raft；不决定 object deleted 可见性

### `StorageNodeService::BatchDeleteChunks(...)`

- 责任：承接 gRPC `BatchDeleteChunks` RPC，按请求顺序逐项调用 `ChunkStore::DeleteChunk()`，并聚合 top-level 计数和 partial failure 事实
- 输入：gRPC context、proto `BatchDeleteChunksRequest`
- 输出：proto `BatchDeleteChunksResponse`
- 边界：逐项结果独立返回；校验失败或 non-retryable item 不会污染已成功项；当前不做后台 GC 或并发删除调度

## storage_node_client.cpp 关键 helper

### `ResolveAbsoluteDeadline(const StorageTaskContext&, std::chrono::system_clock::time_point)`

- 责任：把本地 `timeout_ms` 转成整次 RPC 调用共享的绝对 deadline
- 输入：本地 task context、调用开始时间
- 输出：`ClientContext` 可复用的绝对 deadline
- 边界：`timeout_ms == 0` 时返回无穷 deadline，不伪造超时

### `ApplyDeadlineToContext(const StorageTaskContext&, std::chrono::system_clock::time_point, grpc::ClientContext*)`

- 责任：把绝对 deadline 写入 gRPC `ClientContext`
- 输入：本地 task context、绝对 deadline、gRPC context
- 输出：带 deadline 的 `ClientContext`
- 边界：只约束 RPC 生命周期，不承诺 service/store 具备运行中取消传播

### `FillProtoReadRequest(const ReadChunkRequest&, const StorageNodeClientReadChunkOptions&, storage::ReadChunkRequest*)`

- 责任：把本地 `storedemo::ReadChunkRequest` 转成 proto `ReadChunkRequest`
- 输入：本地 read request、client read options、proto 输出指针
- 输出：填充 `request_id`、`chunk_id`、`offset/length`、`expected_checksum`、`timeout_ms`、`best_effort_cancel`、`verify_checksum`
- 边界：本地请求当前只有 `chunk_id` 身份语义，不额外发明 `object_id/version/chunk_index`

### `FillProtoDeleteRequest(const StorageNodeClientDeleteChunkRequest&, const StorageNodeClientDeleteChunkOptions&, storage::DeleteChunkRequest*)`

- 责任：把本地单 chunk 删除请求转成 proto `DeleteChunkRequest`
- 输入：本地 delete request、client delete options、proto 输出指针
- 输出：填充 `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`expected_checksum`、`reason`、`metadata_boundary`、`timeout_ms`、`best_effort_cancel`
- 边界：只表达 chunk data-plane 删除身份和调用上下文，不决定 object deleted 可见性

### `FillProtoBatchDeleteRequest(const StorageNodeClientBatchDeleteChunksRequest&, const StorageNodeClientDeleteChunkOptions&, storage::BatchDeleteChunksRequest*)`

- 责任：把本地批删请求转成 proto `BatchDeleteChunksRequest`
- 输入：本地 batch delete request、client delete options、proto 输出指针
- 输出：填充 top-level `request_id/timeout_ms/best_effort_cancel` 和逐项 `chunk_id/object_id/version/chunk_index/expected_checksum/reason/metadata_boundary`
- 边界：逐项 request 顺序保持不变；不在 client 层发明 batch-only 状态语义

### `MakeReadChunkRequestForCommittedManifestReplica(std::string_view, std::string_view, std::uint64_t, std::string_view)`

- 责任：把 committed manifest 中的 `chunk_id / size / checksum` 收口成一次完整 full-read 请求
- 输入：上层生成的 `request_id`、manifest `chunk_id`、manifest `size`、manifest `checksum`
- 输出：本地 `ReadChunkRequest`
- 边界：固定生成 `verify_checksum=true`、`expected_checksum.algorithm=SHA256` 的 full read；不决定 object committed 可见性，也不引入 range 语义

### `ClassifyReadReplicaFailure(const ReadChunkResponse&)`

- 责任：把单副本读失败分类为“停止”或“继续尝试下一副本”
- 输入：单次副本读返回的本地 `ReadChunkResponse`
- 输出：`ReadReplicaFailureAction`
- 当前继续尝试的状态：
  - `TIMEOUT`
  - `NODE_UNAVAILABLE`
  - `OVERLOADED`
  - `IO_ERROR`
  - `NOT_FOUND`
  - `CONFLICT`
  - `CHECKSUM_MISMATCH`
  - `CORRUPTED`
- 当前停止的状态：
  - `INVALID_ARGUMENT`
  - `CANCELLED`
  - `UNSUPPORTED`
  - `PERMISSION_DENIED`
  - 以及其余明显不适合继续扩散的状态
- 边界：这里只做客户端侧 fallback 分类，不做副本坏块写回或 repair 调度

### `ReadChunkWithReplicaFallback(std::span<const StorageNodeId>, const ReadChunkRequest&, StorageNodeClientReadChunkOptions, const ReadChunkReplicaInvoker&)`

- 责任：按上层已经选好的副本顺序逐个尝试读取，成功即返回，遇到不可继续错误则立即停止
- 输入：有序 `replica_nodes`、本地读请求、读选项、调用方提供的单副本 invoker
- 输出：`ReadReplicaFallbackResult`
- 当前行为：
  - 空副本列表或空 invoker 直接返回 `kInvalidArgument`
  - 成功时记录 `selected_node_id`
  - 可继续错误则进入下一个副本
  - 所有可继续副本都失败后，会按“checksum/corrupted 优先于 conflict/not_found，再优先于瞬时错误”的原则聚合一个明确失败
- 边界：
  - 不做 replica selection，顺序完全由上层提供
  - 不接 metadata / Raft
  - 不做后台 repair、坏副本写回或健康状态持久化

### `ResolveReadResponseIdentity(const ReadChunkRequest&, const storage::ReadChunkResponse&, ChunkIdentity*, std::string*)`

- 责任：从 proto `chunk_id` / `summary.chunk_id` / 本地请求中恢复本地 `ChunkIdentity`
- 输入：本地 read request、proto read response
- 输出：尽量完整的 `ChunkIdentity`
- 边界：服务端返回无效 chunk id 视为协议错误并返回失败；只有请求本身无法解析时，才退化为仅保留原始 `chunk_id`

### `ResolveDeleteResponseIdentity(std::string_view, std::string_view, std::uint64_t, std::uint32_t, std::string_view, std::string_view, ChunkIdentity*, std::string*)`

- 责任：从 proto `summary.chunk_id` / 顶层 `chunk_id` / 本地 delete request 中恢复本地 `ChunkIdentity`
- 输入：本地 delete request 的 `chunk_id/object_id/version/chunk_index`、proto summary/top-level chunk id
- 输出：尽量完整的 `ChunkIdentity`
- 边界：服务端返回无效 chunk id 视为协议错误；若服务端不回 chunk id，则按本地请求回填或派生，不在 client 层发明 metadata 可见性

### `TranslateProtoReadResponse(const ReadChunkRequest&, const storage::ReadChunkResponse&)`

- 责任：把 proto `ReadChunkResponse` 转回本地 `storedemo::ReadChunkResponse`
- 输入：本地 read request、proto read response
- 输出：本地 `status`、`payload`、`metadata`、`actual_checksum`、`verified`
- 边界：成功响应若 `complete/full_read` 与本地请求语义矛盾，会显式返回 `IO_ERROR`，避免 silent partial success

### `MakeGrpcReadFailureResponse(const grpc::Status&)`

- 责任：把非 OK gRPC status 映射成明确的本地读错误
- 输入：gRPC status
- 输出：本地 `ReadChunkResponse`
- 边界：`DEADLINE_EXCEEDED/CANCELLED/UNAVAILABLE` 保持明确映射，其它失败默认收口到本地 `IO_ERROR`

### `TranslateProtoDeleteResponse(const StorageNodeClientDeleteChunkRequest&, const storage::DeleteChunkResponse&)`

- 责任：把 proto `DeleteChunkResponse` 转回本地 `storedemo::StorageNodeClientDeleteChunkResponse`
- 输入：本地 delete request、proto delete response
- 输出：本地 `status`、`retry_after_ms`、`metadata`、`deleted/already_missing/already_deleted/retryable`
- 边界：`already_deleted` 会在 `already_missing + DELETED` 语义上补齐；identity/checksum/state 非法时显式映射成 `IO_ERROR`

### `TranslateProtoBatchDeleteResponse(const StorageNodeClientBatchDeleteChunksRequest&, const storage::BatchDeleteChunksResponse&)`

- 责任：把 proto `BatchDeleteChunksResponse` 转回本地 `storedemo::StorageNodeClientBatchDeleteChunksResponse`
- 输入：本地 batch delete request、proto batch delete response
- 输出：top-level `status/error_detail/retry_after_ms`、逐项结果、success/idempotent/retryable/non-retryable 计数和 `partial_failure`
- 边界：会校验逐项结果数量和聚合计数是否与逐项事实一致；不一致时显式映射成 `IO_ERROR`

### `MakeGrpcDeleteFailureResponse(const grpc::Status&)`

- 责任：把非 OK gRPC status 映射成明确的本地单删错误
- 输入：gRPC status
- 输出：本地 `StorageNodeClientDeleteChunkResponse`
- 边界：`DEADLINE_EXCEEDED` 映射为 timeout，`CANCELLED` 映射为 cancelled，`UNAVAILABLE` 映射为 node unavailable；其余失败明确收口，不做 silent success

### `MakeGrpcBatchDeleteFailureResponse(const grpc::Status&)`

- 责任：把非 OK gRPC status 映射成明确的本地批删错误
- 输入：gRPC status
- 输出：本地 `StorageNodeClientBatchDeleteChunksResponse`
- 边界：只给出 top-level RPC 失败，不伪造逐项结果或后台重试结论

### `StorageNodeClient::ReadChunk(...)`

- 责任：组装 proto request、设置 deadline、发起同步 `ReadChunk` RPC，并把结果转回本地 read response
- 输入：本地 `ReadChunkRequest`、client read options
- 输出：本地 `ReadChunkResponse`
- 边界：不调用 metadata / Raft；不决定 object committed 可见性；自身不做 replica selection，只承担单副本 RPC 适配

### `StorageNodeClient::DeleteChunk(...)`

- 责任：组装 proto delete request、设置 deadline、发起同步 `DeleteChunk` RPC，并把结果转回本地单删响应
- 输入：本地 `StorageNodeClientDeleteChunkRequest` 或兼容的 `DeleteChunkRequest`、client delete options
- 输出：本地 `StorageNodeClientDeleteChunkResponse`
- 边界：不调用 metadata / Raft；不决定 object deleted 可见性；当前保持单次 RPC，不自动重试

### `StorageNodeClient::BatchDeleteChunks(...)`

- 责任：组装 proto batch delete request、设置 deadline、发起同步 `BatchDeleteChunks` RPC，并把逐项结果与聚合事实转回本地批删响应
- 输入：本地 `StorageNodeClientBatchDeleteChunksRequest`、client delete options
- 输出：本地 `StorageNodeClientBatchDeleteChunksResponse`
- 边界：不调用 metadata / Raft；不决定 object deleted 可见性；当前不自动重试 retryable item，只把后续重试所需分类事实返回给调用方

## T082 ScrubChunk 适配边界

### `TranslateScrubRequest(const storage::ScrubChunkRequest&, ScrubChunkRequestContext*)`

- 责任：把 proto `ScrubChunkRequest` 转成本地 `StatChunk` 请求上下文
- 输入：proto scrub request、本地 scrub 请求上下文输出
- 输出：`request_id/chunk_id`、期望 checksum/size，以及后续 pre-verify/post-stat 复用的 `StatChunkRequest`
- 边界：当前强制走 checksum verify 路径；`quarantine_on_corruption` 只是 contract 字段透传边界，不能抑制 `LocalDiskChunkStore::StatChunk(verify_checksum=true)` 触发的 T072 quarantine

### `CompareScrubExpectedChecksum(...)`

- 责任：比较 scrub request 的 expected checksum 与已验证的 observed checksum
- 输入：expected checksum、observed checksum
- 输出：`OK` 或 `CHECKSUM_MISMATCH`
- 边界：这里只比较 chunk bytes 事实，不决定 object committed/deleted 可见性，也不把 payload 写入 metadata / Raft

### `FillScrubFact(...)` / `FillScrubResponse(...)`

- 责任：把本地 scrub 结果回填成 proto `ScrubChunkFact` / `ScrubChunkResponse`
- 输入：proto scrub request、本地 scrub service 结果、configured node id
- 输出：`chunk_id`、expected/observed checksum/size、`state_before/state_after`、`known_corrupted/known_missing/quarantined`、`repair_required/retryable`
- 边界：只表达本地 chunk data-plane 事实，不做 repair，不改 metadata manifest，不调用 Raft

### `StorageNodeService::ScrubChunk(...)`

- 责任：按 `pre-stat -> verify-stat -> post-stat` 顺序执行最小生产 scrub 链路，并把 healthy/missing/corrupted/quarantined 边界映射成 proto 响应
- 输入：gRPC `ScrubChunkRequest`
- 输出：gRPC `ScrubChunkResponse`
- 边界：当前通过注入的 `ChunkStore::StatChunk()` 复用本地 checksum/quarantine 语义；missing 返回 `NOT_FOUND`，quarantined/corrupted 归一成不可作为 healthy source 的 `CORRUPTED` 语义；不触碰 metadata / Raft

### `FillProtoScrubRequest(...)`

- 责任：把本地 `StorageNodeClientScrubChunkRequest` 转成 proto `ScrubChunkRequest`
- 输入：本地 scrub request、client scrub options
- 输出：带 `timeout_ms/best_effort_cancel/verify_checksum/quarantine_on_corruption` 的 proto request
- 边界：client 只做字段映射，不伪装 manager、repair 或 manifest coordination 已接线

### `TranslateProtoScrubResponse(...)`

- 责任：把 proto `ScrubChunkResponse` 转回本地 `StorageNodeClientScrubChunkResponse`
- 输入：本地 scrub request、proto scrub response
- 输出：本地 `status`、`metadata`、expected/observed checksum/size、before/after state、corrupted/missing/quarantine/repair facts
- 边界：服务端返回非法 chunk id/checksum/state 会显式映射成 `IO_ERROR`，避免 silent success

### `MakeGrpcScrubFailureResponse(const grpc::Status&)`

- 责任：把非 OK gRPC status 映射成明确的本地 scrub 错误
- 输入：gRPC status
- 输出：本地 `StorageNodeClientScrubChunkResponse`
- 边界：`DEADLINE_EXCEEDED -> TIMEOUT`、`CANCELLED -> CANCELLED`、`UNAVAILABLE -> NODE_UNAVAILABLE`；`best_effort_cancel` 仍不代表运行中取消传播

### `StorageNodeClient::ScrubChunk(...)`

- 责任：组装 proto scrub request、设置 deadline、发起同步 `ScrubChunk` RPC，并把 proto/gRPC 结果转回本地 scrub 响应
- 输入：本地 `StorageNodeClientScrubChunkRequest`、client scrub options
- 输出：本地 `StorageNodeClientScrubChunkResponse`
- 边界：不调用 metadata / Raft；不决定 object committed/deleted 可见性；当前只做单节点 scrub RPC 适配，不伪装生产 ScrubManager 已完成

## 后续演进

- T045 已补最小 committed-manifest 读 fallback helper；T053 已补 client 侧 `DeleteChunk` / `BatchDeleteChunks` 适配；T082 已补最小 `ScrubChunk` service/client 适配；后续仍需继续补 registry/heartbeat facts 接入、`RepairChunk`/manager、read replica health 演进、生产 GC/restart cleanup，以及 `ListChunks`
