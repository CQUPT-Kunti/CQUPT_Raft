# store/node 说明

## 模块职责

`modules/store/node` 承载 StorageNode data-plane 的 RPC 适配层。

当前只负责：

- `StorageNodeService::WriteChunk` 的 gRPC `WriteChunk` 入口
- `StorageNodeService::ReadChunk` 的 gRPC `ReadChunk` 入口
- `StorageNodeClient::WriteChunk` 的本地请求到 gRPC `WriteChunk` 调用
- `StorageNodeClient::ReadChunk` 的本地请求到 gRPC `ReadChunk` 调用
- proto `WriteChunkRequest` 到 `ChunkStore::WriteChunk` 的字段转换
- proto `ReadChunkRequest` 到 `ChunkStore::ReadChunk` 的字段转换
- `ChunkStore` 结果到 `storage_node.proto` 响应的状态码、checksum、state、durable、already_exists 映射
- `ChunkStore` 的 `ReadChunkResponse` 到 `storage_node.proto::ReadChunkResponse` 的状态码、checksum、state、payload、offset、complete/full_read 映射
- `storage_node.proto`（`package storage`）`WriteChunkResponse` 和 gRPC status 到本地 `storedemo::WriteChunkResponse` / `StorageNodeStatusCode` 的映射
- `storage_node.proto`（`package storage`）`ReadChunkResponse` 和 gRPC status 到本地 `storedemo::ReadChunkResponse` / `StorageNodeStatusCode` 的映射

当前不负责：

- `DeleteChunk` / `StatChunk` / `ListChunks`
- metadata `CreateObject` / `CommitObject` / `AbortObject`
- `RaftNode::ProposeMetadata()`
- upload coordinator
- gRPC server 启动与进程生命周期管理

## 主要文件

- `storage_node_service.h`：service 类声明
- `storage_node_service.cpp`：`WriteChunk` / `ReadChunk` 适配实现和 proto/store 映射 helper
- `storage_node_client.h`：client 类声明以及 write/read 选项结构
- `storage_node_client.cpp`：同步 `WriteChunk` / `ReadChunk` 调用、deadline 设置和响应映射

## T031/T032 固定边界

- `StorageNodeService` 必须通过构造函数注入 `ChunkStore`，不在 service 内部创建全局 `LocalDiskChunkStore`
- `StorageNodeClient` 必须通过构造函数注入 generated stub 或 gRPC channel，不直接依赖 metadata / Raft 模块
- `WriteChunk` 成功只表示 chunk 已经按当前 store contract durable publish，不表示 metadata object 已 committed
- `ReadChunk` 成功只表示当前 chunk data-plane 读取成功，不表示 metadata object 已 committed，也不决定 object 可见性
- service 只调用 `ChunkStore::WriteChunk()`，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- service 的 `ReadChunk` 只调用 `ChunkStore::ReadChunk()`，不调用 metadata service / `MetadataStateMachine` / `RaftNode::ProposeMetadata()`
- client 只调用 `storage_node.proto` 的 `WriteChunk` RPC，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- client 的 `ReadChunk` 只调用 `storage_node.proto` 的 `ReadChunk` RPC，不调用 metadata service / `MetadataStateMachine` / `RaftNode::ProposeMetadata()`
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload` 都会转换到 `ChunkStore::WriteChunk` 请求
- `ReadChunk` 当前会把 `request_id`、`chunk_id`、`expected_checksum`、`verify_checksum` 转到 `ChunkStore::ReadChunk`；如果 `chunk_id` 为空，则尝试用 `object_id + version + chunk_index` 派生 chunk id；如果 `length > 0`，则把 `offset + length` 转成 `ChunkReadRange`
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload`、`timeout_ms`、`best_effort_cancel`、`durability` 都会转换到 `storage_node.proto::WriteChunkRequest`
- client 的 `ReadChunk` 会把本地 `request_id`、`chunk_id`、`range(offset/length)`、`expected_checksum`、`verify_checksum`、`timeout_ms`、`best_effort_cancel` 转换到 `storage_node.proto::ReadChunkRequest`；本地请求没有 `object_id/version/chunk_index` 时，不伪造对象可见性语义
- `storage_node.proto::ReadChunkRequest` 中的 `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，不会在 service/store 内伪装成已经具备运行中取消传播
- proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 都以 `ChunkStore` 返回事实为准
- `ReadChunkResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`chunk_id`、`payload`、`size`、`checksum`、`state`、`offset`、`complete`、`full_read` 都以 `ChunkStore::ReadChunk` 返回事实和当前 request range 语义为准
- client 会把 proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 转回本地 `storedemo::WriteChunkResponse`
- client 会把 proto `ReadChunkResponse.summary.code`、`summary.message`、`summary.retry_after_ms`、`chunk_id`、`payload`、`size`、`checksum`、`state`、`offset` 转回本地 `storedemo::ReadChunkResponse`；如果 proto 成功但 `complete/full_read` 语义与本地请求不一致，会显式映射成 `IO_ERROR`，不做 silent success

## timeout / cancellation / durability 边界

- `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，T031 不会把它们包装成“已经具备运行中取消传播”的既成事实
- 原因是当前 `ChunkStore::WriteChunk` / `ChunkStore::ReadChunk` 接口都还没有 `StorageTaskContext` 入参，runtime 侧也尚未实现真实运行中 deadline/cancellation 传播
- T032 会在 client 侧把 `timeout_ms` 同时写入 proto request，并映射成 gRPC `ClientContext` deadline；当前 deadline 只约束 RPC 生命周期，不意味着 service/store 已具备运行中中断能力
- `best_effort_cancel` 当前只会作为 proto 字段透传，不会在 client 侧伪装为完整的 end-to-end cancellation propagation
- `WriteChunkDurability` 目前只接受 `UNSPECIFIED` 或 `PUBLISH`；当前 `LocalDiskChunkStore::WriteChunk` 的成功语义就是完成 durable publish
- `ReadChunk` 当前如果收到 `length > 0` 的 range request，会原样传给 `ChunkStore::ReadChunk`；是否支持 range 由底层 store 决定。当前 `LocalDiskChunkStore` 会返回显式 `UNSUPPORTED`，service 不做 silent partial success
- `LocalDiskChunkStore::ReadChunk` 当前在 checksum mismatch / corrupted 场景只返回明确错误，不会自动回写 `CORRUPTED` / `QUARANTINED`；service 适配层保持这个边界，不在 node 层强行发明状态回写
- `StorageNodeClient` 当前支持有限自动重试，但只会重试 retryable 状态：`TIMEOUT`、`IO_ERROR`、`OVERLOADED`、`NODE_UNAVAILABLE`；`CONFLICT`、`CHECKSUM_MISMATCH`、`INVALID_ARGUMENT`、`CANCELLED` 等非 retryable 结果不会重试
- client 的 `timeout_ms` 当前作为整次 `WriteChunk` 调用的绝对 deadline 预算；每次重试共用同一个 deadline，不会无限延长总等待时间
- `StorageNodeClient::ReadChunk` 当前只做单副本、单次 RPC 读取；T044 不实现 read retry、read fallback 或 replica selection，这些由 T045 之后处理

## storage_node_service.cpp 关键 helper

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

### `ResolveReadResponseIdentity(const ReadChunkRequest&, const storage::ReadChunkResponse&, ChunkIdentity*, std::string*)`

- 责任：从 proto `chunk_id` / `summary.chunk_id` / 本地请求中恢复本地 `ChunkIdentity`
- 输入：本地 read request、proto read response
- 输出：尽量完整的 `ChunkIdentity`
- 边界：服务端返回无效 chunk id 视为协议错误并返回失败；只有请求本身无法解析时，才退化为仅保留原始 `chunk_id`

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

### `StorageNodeClient::ReadChunk(...)`

- 责任：组装 proto request、设置 deadline、发起同步 `ReadChunk` RPC，并把结果转回本地 read response
- 输入：本地 `ReadChunkRequest`、client read options
- 输出：本地 `ReadChunkResponse`
- 边界：不调用 metadata / Raft；不决定 object committed 可见性；不做 read fallback / replica selection

## 后续演进

- T045/T052 之后再继续补 read fallback / replica selection、`DeleteChunk` / `StatChunk` / `ListChunks`
