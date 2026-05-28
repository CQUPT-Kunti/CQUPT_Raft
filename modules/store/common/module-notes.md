# store/common 说明

## 这个模块是做什么的

`modules/store/common` 是 `storedemo` 数据面的基础类型模块。

它主要提供两类东西：

1. store 数据面的公共数据结构
2. 不依赖 Raft、proto、KV 的基础 helper

当前它不负责：

- 本地文件落盘
- ChunkStore 接口
- StorageNode RPC
- ChunkIndex 容器实现
- repair / rebalance / executor

## 命名空间

本模块统一使用 `storedemo`。

## 主要数据结构

### `StorageNodeStatusCode`

store 侧统一错误码。

用途：

- 表达参数错误、I/O 错误、checksum mismatch、节点不可用等结果
- 给后续 `WriteChunk`、`ReadChunk`、`Scrub`、`Repair` 共用

说明：

- 它只表达 store 语义
- 不等同于 proto 状态码
- 不等同于 Raft 状态

### `ChunkState`

表示 chunk 在本地节点上的状态。

当前主要状态：

- `kStaging`：正在写，不能对外读
- `kLive`：可读的正常副本
- `kDeleting`：正在删
- `kDeleted`：已删
- `kQuarantined`：文件在，但已隔离
- `kCorrupted`：校验失败或确认损坏
- `kMissing`：预期存在，但本地没找到

### `ChunkChecksum`

保存 checksum 结果。

关键字段：

- `algorithm`：当前使用哪种算法
- `value`：摘要值，当前是小写十六进制 SHA-256
- `size_bytes`：参与计算的 payload 大小
- `computed_at`：预留时间字段，当前 helper 不负责填写

要点：

- 它只表示完整性校验结果
- 不表示内容寻址身份
- 不做全局去重或引用计数
- 空 payload 也允许有合法 checksum

### `ChunkIdentity`

保存 chunk 的逻辑身份。

关键字段：

- `chunk_id`
- `object_id`
- `version`
- `chunk_index`
- `offset`

要点：

- `chunk_id` 是最终对外使用的 chunk 标识
- `object_id + version + chunk_index` 是生成 `chunk_id` 的基础输入
- `offset` 表示它在对象里的偏移，不参与 `chunk_id` 生成

### `ChunkLocation`

轻量位置引用，只表示：

- 这个 chunk 是谁
- 这个 chunk 在哪个 node 上

### `ChunkReplica`

表示一个副本的轻量事实。

常用字段：

- 所在节点
- 大小
- checksum
- 状态
- 最近错误
- 失败次数

用途：

- 后续副本选择
- 健康判断
- repair 输入

### `ChunkMetadata`

表示单个 chunk 在本地节点上的元信息。

它是“本地事实”，不是 metadata source of truth。

特殊字段：

- `write_request_id`：后续写幂等使用
- `delete_request_id`：后续删幂等使用
- `quarantine_reason`：说明为什么被隔离

### `ChunkIndexEntry`

是未来 `ChunkIndex` 单条记录的载体。

特殊字段：

- `final_path`：最终可见路径
- `staging_path`：暂存路径
- `metadata_path`：预留给本地 sidecar
- `lock_shard`：后续分片锁定位用

## 主要 helper

### 状态辅助

- `ToString(StorageNodeStatusCode)`：把错误码转成稳定字符串
- `ToString(ChunkState)`：把状态转成稳定字符串
- `IsRetriableStatus(...)`：判断错误是否适合重试
- `IsReadableChunkState(...)`：判断状态是否可读
- `IsTerminalChunkState(...)`：判断状态是否是终止态

## chunk_id helper

### 规则

当前规则：

`object_id~version~chunk_index`

为什么这么做：

- 能稳定定位“某个对象版本的某个 chunk”
- 便于幂等写、manifest 查询、GC、repair

为什么分隔符是 `~`：

- `chunk_id` 后续可能进入本地文件路径
- `:` 这类字符跨平台不安全

### 相关函数

- `ValidateChunkObjectId(...)`：校验 `object_id` 是否可用于生成 `chunk_id`
- `MakeChunkId(...)`：生成合法 `chunk_id`
- `ParseChunkId(...)`：把 `chunk_id` 解析回 `ChunkIdentity`
- `ValidateChunkId(...)`：只校验，不解析输出

### 特殊约束

- `object_id` 不能为空
- `object_id` 不能包含路径逃逸或路径分隔符
- `version` 必须大于 0
- `chunk_id` 总长度受控，便于后续本地文件布局

## checksum helper

### 作用

checksum helper 只做一件事：数据完整性校验。

它不做：

- 内容寻址
- 全局去重
- refcount

### 当前实现

- 算法：`SHA-256`
- 输出格式：小写十六进制字符串

### 相关函数

- `ComputeChunkChecksum(...)`：对 payload 计算 checksum
- `VerifyChunkChecksum(...)`：拿 expected checksum 校验 payload

### 返回语义

- 匹配：`kOk`
- 不匹配：`kChecksumMismatch`
- 参数或算法非法：返回明确错误码

## 维护建议

后面如果 `modules/store/` 下新增子模块，建议每个子模块都放一份类似说明，重点写：

- 这个子模块干什么
- 核心结构/类/函数
- 特殊字段
- 容易误用的限制
