# store/common 说明

## 模块职责

`modules/store/common` 是 `storedemo` 数据面的基础类型模块。

这里放两类内容：

- 公共数据结构
- 不依赖 Raft、proto、KV 的基础 helper

这里不做：

- 本地文件落盘
- `ChunkStore` 业务实现
- StorageNode RPC
- ChunkIndex 容器实现

## 文件对照

- `store_types.h`：公开枚举、结构体、常量、helper 声明
- `store_types.cpp`：helper 具体实现

如果你想看“文档里的函数到底落在哪”，直接对这两个文件即可。

## 主要结构体

### `StorageNodeStatusCode`

store 侧统一错误码。

给后续 `WriteChunk`、`ReadChunk`、`Scrub`、`Repair` 共用，不等同于 proto 状态码，也不等同于 Raft 状态。

### `ChunkState`

表示 chunk 在本地节点上的状态。

- `kStaging`：正在写，不能对外读
- `kLive`：可读副本
- `kDeleting` / `kDeleted`：删除流程
- `kQuarantined` / `kCorrupted`：坏块或隔离块
- `kMissing`：预期存在但本地没找到

### `ChunkChecksum`

保存完整性校验结果。

关键字段：

- `algorithm`：当前算法
- `value`：摘要字符串
- `size_bytes`：参与计算的 payload 大小
- `computed_at`：预留时间字段

它只表示完整性校验，不表示内容寻址身份，也不做去重。

### `ChunkIdentity`

保存 chunk 的逻辑身份。

关键字段：

- `chunk_id`
- `object_id`
- `version`
- `chunk_index`
- `offset`

其中 `chunk_id` 基于 `object_id + version + chunk_index` 生成，`offset` 不参与生成。

### `ChunkLocation`

轻量位置引用，只表达“哪个 node 上的哪个 chunk”。

### `ChunkReplica`

表示副本的轻量事实，后续给副本选择、健康判断和 repair 复用。

### `ChunkMetadata`

表示本地节点看到的 chunk 元信息，不是全局 metadata source of truth。

特殊字段：

- `write_request_id`
- `delete_request_id`
- `quarantine_reason`

### `ChunkIndexEntry`

未来 `ChunkIndex` 单条记录的载体。

特殊字段：

- `final_path`
- `staging_path`
- `metadata_path`
- `lock_shard`

## `.cpp` 里当前实现了哪些函数

下面这些函数都在 `store_types.cpp` 里有对应实现：

### 状态和字符串 helper

- `ToString(StorageNodeStatusCode)`：错误码转稳定字符串
- `ToString(ChunkState)`：状态转稳定字符串
- `IsRetriableStatus(...)`：判断是否适合重试
- `IsReadableChunkState(...)`：判断状态是否可读
- `IsTerminalChunkState(...)`：判断状态是否终止

### chunk_id helper

规则是：

`object_id~version~chunk_index`

对应实现函数：

- `ValidateChunkObjectId(...)`：校验 `object_id`
- `MakeChunkId(...)`：生成 `chunk_id`
- `ParseChunkId(...)`：把 `chunk_id` 解析回 `ChunkIdentity`
- `ValidateChunkId(...)`：只校验，不返回解析结果

使用限制：

- `object_id` 不能为空
- 不能包含路径分隔符、`..` 或危险字符
- `version` 必须大于 0
- 总长度受控，便于后续本地路径布局

### checksum helper

对应实现函数：

- `ComputeChunkChecksum(...)`：对 payload 计算 checksum
- `VerifyChunkChecksum(...)`：拿 expected checksum 校验 payload

当前实现使用 SHA-256，小写十六进制输出。

### 结构体上的成员函数

这些成员函数也都在 `store_types.cpp` 里：

- `ChunkLocation::IsValid()`
- `ChunkChecksum::IsSet()`
- `ChunkIdentity::HasChunkKey()`
- `ChunkReplica::IsReadable()`
- `ChunkMetadata::IsReadable()`
- `ChunkIndexEntry::HasFinalPath()`

## 额外说明

- `store_types.cpp` 里还有 SHA-256 的内部匿名命名空间 helper，它们只服务 `ComputeChunkChecksum(...)`，不是对外 API。
- 如果这里新增结构体、字段或 helper，需要同时维护本说明，让文档名词能直接对上头文件和 `.cpp` 函数。
