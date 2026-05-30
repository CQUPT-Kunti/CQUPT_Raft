# store/node 说明

## 模块职责

`modules/store/node` 承载 StorageNode data-plane 的 RPC 适配层。

当前只负责：

- `StorageNodeService::WriteChunk` 的 gRPC `WriteChunk` 入口
- `StorageNodeClient::WriteChunk` 的本地请求到 gRPC `WriteChunk` 调用
- proto `WriteChunkRequest` 到 `ChunkStore::WriteChunk` 的字段转换
- `ChunkStore` 结果到 `storage_node.proto` 响应的状态码、checksum、state、durable、already_exists 映射
- `storage_node.proto` `WriteChunkResponse` 和 gRPC status 到本地 `storedemo::WriteChunkResponse` / `StorageNodeStatusCode` 的映射

当前不负责：

- `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks`
- metadata `CreateObject` / `CommitObject` / `AbortObject`
- `RaftNode::ProposeMetadata()`
- upload coordinator
- gRPC server 启动与进程生命周期管理

## 主要文件

- `storage_node_service.h`：service 类声明
- `storage_node_service.cpp`：`WriteChunk` 适配实现和 proto/store 映射 helper
- `storage_node_client.h`：client 类声明和 write 选项结构
- `storage_node_client.cpp`：同步 `WriteChunk` 调用、deadline 设置、重试和响应映射

## T031/T032 固定边界

- `StorageNodeService` 必须通过构造函数注入 `ChunkStore`，不在 service 内部创建全局 `LocalDiskChunkStore`
- `StorageNodeClient` 必须通过构造函数注入 generated stub 或 gRPC channel，不直接依赖 metadata / Raft 模块
- `WriteChunk` 成功只表示 chunk 已经按当前 store contract durable publish，不表示 metadata object 已 committed
- service 只调用 `ChunkStore::WriteChunk()`，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- client 只调用 `storage_node.proto` 的 `WriteChunk` RPC，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload` 都会转换到 `ChunkStore::WriteChunk` 请求
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload`、`timeout_ms`、`best_effort_cancel`、`durability` 都会转换到 `storage_node.proto::WriteChunkRequest`
- proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 都以 `ChunkStore` 返回事实为准
- client 会把 proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 转回本地 `storedemo::WriteChunkResponse`

## timeout / cancellation / durability 边界

- `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，T031 不会把它们包装成“已经具备运行中取消传播”的既成事实
- 原因是当前 `ChunkStore::WriteChunk` 接口还没有 `StorageTaskContext` 入参，runtime 侧也尚未实现真实运行中 deadline/cancellation 传播
- T032 会在 client 侧把 `timeout_ms` 同时写入 proto request，并映射成 gRPC `ClientContext` deadline；当前 deadline 只约束 RPC 生命周期，不意味着 service/store 已具备运行中中断能力
- `best_effort_cancel` 当前只会作为 proto 字段透传，不会在 client 侧伪装为完整的 end-to-end cancellation propagation
- `WriteChunkDurability` 目前只接受 `UNSPECIFIED` 或 `PUBLISH`；当前 `LocalDiskChunkStore::WriteChunk` 的成功语义就是完成 durable publish
- `StorageNodeClient` 当前支持有限自动重试，但只会重试 retryable 状态：`TIMEOUT`、`IO_ERROR`、`OVERLOADED`、`NODE_UNAVAILABLE`；`CONFLICT`、`CHECKSUM_MISMATCH`、`INVALID_ARGUMENT`、`CANCELLED` 等非 retryable 结果不会重试
- client 的 `timeout_ms` 当前作为整次 `WriteChunk` 调用的绝对 deadline 预算；每次重试共用同一个 deadline，不会无限延长总等待时间

## 后续演进

- T042/T043/T052 之后再按同一模式补 `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks`
