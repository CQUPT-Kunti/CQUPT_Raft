# store/node 说明

## 模块职责

`modules/store/node` 承载 StorageNode data-plane 的 RPC 适配层。

当前只负责：

- `StorageNodeService::WriteChunk` 的 gRPC `WriteChunk` 入口
- proto `WriteChunkRequest` 到 `ChunkStore::WriteChunk` 的字段转换
- `ChunkStore` 结果到 `storage_node.proto` 响应的状态码、checksum、state、durable、already_exists 映射

当前不负责：

- `StorageNodeClient`
- `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks`
- metadata `CreateObject` / `CommitObject` / `AbortObject`
- `RaftNode::ProposeMetadata()`
- upload coordinator
- gRPC server 启动与进程生命周期管理

## 主要文件

- `storage_node_service.h`：service 类声明
- `storage_node_service.cpp`：`WriteChunk` 适配实现和 proto/store 映射 helper

## T031 固定边界

- `StorageNodeService` 必须通过构造函数注入 `ChunkStore`，不在 service 内部创建全局 `LocalDiskChunkStore`
- `WriteChunk` 成功只表示 chunk 已经按当前 store contract durable publish，不表示 metadata object 已 committed
- service 只调用 `ChunkStore::WriteChunk()`，不触发 metadata commit，也不调用 `RaftNode::ProposeMetadata()`
- `request_id`、`chunk_id`、`object_id`、`version`、`chunk_index`、`offset`、`expected_size`、`expected_checksum`、`payload` 都会转换到 `ChunkStore::WriteChunk` 请求
- proto `summary.code`、`summary.message`、`summary.retry_after_ms`、`durable`、`already_exists`、`size`、`checksum`、`state` 都以 `ChunkStore` 返回事实为准

## timeout / cancellation / durability 边界

- `timeout_ms` 和 `best_effort_cancel` 当前只作为 RPC contract 字段接收，T031 不会把它们包装成“已经具备运行中取消传播”的既成事实
- 原因是当前 `ChunkStore::WriteChunk` 接口还没有 `StorageTaskContext` 入参，runtime 侧也尚未实现真实运行中 deadline/cancellation 传播
- `WriteChunkDurability` 目前只接受 `UNSPECIFIED` 或 `PUBLISH`；当前 `LocalDiskChunkStore::WriteChunk` 的成功语义就是完成 durable publish

## 后续演进

- T032 再补 `StorageNodeClient::WriteChunk`
- T042/T043/T052 之后再按同一模式补 `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks`
