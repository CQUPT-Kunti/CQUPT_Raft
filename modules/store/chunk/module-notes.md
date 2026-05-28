# store/chunk 说明

## 这个模块是做什么的

`modules/store/chunk` 定义 chunk data-plane 的抽象接口边界。

它当前只负责：

- `ChunkStore` 抽象接口
- chunk 请求/响应结构
- 与 `store/common` 类型的拼装边界

它当前不负责：

- 本地磁盘读写
- durable publish
- ChunkIndex 容器
- StorageNode RPC
- repair / rebalance / GC 后台能力

## 命名空间

本模块统一使用 `storedemo`。

## 主要类型

### `ChunkStoreResult`

所有 `ChunkStore` 响应共享的基础结果。

关键字段：

- `status`
- `error_detail`
- `retry_after_ms`

### `ChunkReadRange`

描述一次 read 的局部范围。

字段很少：

- `offset`
- `length`

如果 range 没设置，表示读取整个 chunk。

### `ListChunksOptions`

描述分页和过滤条件。

当前主要字段：

- `state_filter`
- `prefix_filter`
- `page_token`
- `page_size`
- `include_quarantine`

### `WriteChunkRequest / Response`

写入一个 chunk。

特殊字段：

- `request_id`：给幂等重试用
- `identity`：承载 chunk 逻辑身份
- `expected_size`
- `expected_checksum`
- `payload`

当前 `payload` 仍用 `std::string`，是为了让后续 store 实现和 RPC 适配先复用同一套承载方式。

### `ReadChunkRequest / Response`

读取一个 chunk。

特殊字段：

- `range`
- `expected_checksum`
- `verify_checksum`

`verify_checksum=true` 时，表示读路径需要显式做完整性校验。

### `DeleteChunkRequest / Response`

删除一个 chunk。

特殊字段：

- `reason`
- `metadata_boundary`
- `expected_checksum`

`metadata_boundary` 是控制面给数据面的安全边界，不是本模块自己生成的字段。

### `StatChunkRequest / Response`

查询单个 chunk 的状态和元信息。

### `ListChunksRequest / Response`

分页列举 chunk，给后续本地扫描、诊断、GC、repair 使用。

### `ChunkStore`

chunk data-plane 抽象接口。

当前定义的函数：

- `WriteChunk(...)`
- `ReadChunk(...)`
- `DeleteChunk(...)`
- `StatChunk(...)`
- `ListChunks(...)`

## 与其它模块的边界

- 复用 `store/common/store_types.h` 里的 `ChunkId`、`ChunkIdentity`、`ChunkMetadata`、`ChunkChecksum`、`ChunkState`、`StorageNodeStatusCode`
- 后续 `modules/store/io` 提供 durable file 能力
- 后续 `LocalDiskChunkStore` 负责把这里的抽象接口落到本地文件和本地索引
- 后续 `StorageNodeService` 只负责把 RPC 映射到 `ChunkStore`

## 当前未实现内容

- 本地文件 IO
- flush / publish / directory sync
- checksum on write / on read 的实际流程串接
- quarantine / repair / GC / restart recovery
