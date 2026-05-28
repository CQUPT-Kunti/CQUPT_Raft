# store/chunk 说明

## 模块职责

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

## 文件对照

- `chunk_store.h`：接口和 request / response 结构
- `chunk_store.cpp`：当前只有 `ChunkStore::~ChunkStore()`

这个模块现在本来就以头文件声明为主，所以你在 `.cpp` 里看不到很多对应函数是正常的，不是漏维护。

## 主要结构体和类

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

这些函数当前都只在 `chunk_store.h` 里作为纯虚接口声明，真正实现会放到后续 `LocalDiskChunkStore` 一类具体类里。

## `.cpp` 当前实现了什么

- `ChunkStore::~ChunkStore()`

也就是说，当前 `chunk_store.cpp` 的职责只是提供虚析构定义，避免接口类链接缺口。

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
