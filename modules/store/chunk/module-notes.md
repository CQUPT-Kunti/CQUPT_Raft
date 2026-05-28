# store/chunk

## 模块职责

`modules/store/chunk` 定义 chunk data-plane 的抽象接口边界。
这里不做本地磁盘读写、不做 durable publish、不做 ChunkIndex 容器，只描述后续实现需要遵守的请求/响应语义。

## 主要类型

- `ChunkStore`
  - 数据面抽象接口，统一定义 `WriteChunk`、`ReadChunk`、`DeleteChunk`、`StatChunk`、`ListChunks`。
- `ChunkStoreResult`
  - 所有响应共享的基础结果，包含 `status`、`error_detail`、`retry_after_ms`。
- `WriteChunkRequest` / `WriteChunkResponse`
  - 写入一个 chunk。
  - `request_id` 用于幂等重试。
  - `expected_size` 和 `expected_checksum` 用于防止静默写入错误内容。
  - `payload` 当前用 `std::string` 承载二进制字节，后续实现和 RPC 适配都可以直接复用。
- `ReadChunkRequest` / `ReadChunkResponse`
  - 读取一个 chunk。
  - `range` 不为空时表示返回局部字节范围。
  - `verify_checksum=true` 时要求实现侧做完整性校验。
- `DeleteChunkRequest` / `DeleteChunkResponse`
  - 删除一个 chunk。
  - `metadata_boundary` 是控制面传下来的安全边界，防止数据面越界删掉仍被 manifest 引用的数据。
- `StatChunkRequest` / `StatChunkResponse`
  - 查询单个 chunk 的状态和元信息。
- `ListChunksRequest` / `ListChunksResponse`
  - 分页列举 chunk。
  - `page_token` 只用于续页，`page_size=0` 表示交给实现侧选择默认值。

## 与 common / 后续模块的边界

- 复用 `store/common/store_types.h` 里的 `ChunkId`、`ChunkIdentity`、`ChunkMetadata`、`ChunkChecksum`、`ChunkState`、`StorageNodeStatusCode`。
- 后续 `LocalDiskChunkStore` 负责把这些接口落到本地目录、durable file 和 ChunkIndex。
- 后续 `StorageNodeService` 只负责把 RPC 请求映射到 `ChunkStore`，不负责重新定义数据面语义。

## 当前未实现内容

- 本地文件 IO
- durable file flush / publish
- ChunkIndex 和并发锁
- 校验失败后的 quarantine / repair / GC 后台流程
