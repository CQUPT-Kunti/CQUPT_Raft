# store/io 说明

## 这个模块是做什么的

`modules/store/io` 定义 store 数据面的 durable file 抽象接口。

它只负责描述这些能力：

- staging 文件写入
- flush / durability boundary
- staging 到 final 的 atomic publish
- parent directory sync
- 路径规范化
- 跨平台共享错误分类

它当前不负责：

- Linux `fsync` / `fdatasync` 实现
- Windows `FlushFileBuffers` / `MoveFileEx` / `ReplaceFile` 实现
- LocalDiskChunkStore 业务流程
- ChunkIndex 容器
- StorageNodeService / Placement / Repair / Rebalance

## 命名空间

本模块统一使用 `storedemo`。

## 主要类型

### `DurableFileErrorCode`

durable file 层自己的错误分类。

它比 `StorageNodeStatusCode` 更细，补了后续文件层需要区分的错误：

- `kPartialWrite`
- `kPathInvalid`
- `kAtomicPublishFailed`
- `kDirectorySyncFailed`

同时保留通用错误：

- `kDiskFull`
- `kPermissionDenied`
- `kIoError`
- `kChecksumMismatch`
- `kCorrupted`
- `kTimeout`
- `kCancelled`
- `kUnsupported`

### `DurableOperationContext`

给后续实现预留操作控制信息。

当前字段：

- `timeout_ms`
- `best_effort_cancel`

这里先只定义接口，不实现超时和取消机制。

### `DurableFileResult`

durable file 层统一返回结构。

关键字段：

- `error`
- `error_detail`
- `retry_after_ms`
- `bytes_transferred`
- `durable_boundary_reached`
- `partial_progress`

用途：

- 让后续 `LocalDiskChunkStore`、`StorageNodeService`、测试统一看文件层结果
- 把“是否到达 durability 边界”和“是否发生部分进度”单独表达出来

### `NormalizeDurablePathRequest / Response`

用于路径规范化和路径合法性校验。

这里只处理相对路径语义，不绑定 Raft 路径，也不暴露平台句柄。

### `OpenStagingWriterRequest`

用于打开 staging writer。

关键字段：

- `relative_path`
- `expected_size`
- `context`

`expected_size` 是给后续 short write / partial write / size 校验预留的。

### `DurableAppendRequest`

写入请求当前用 `std::span<const std::byte>` 承载字节视图。

这样做的原因很简单：

- 不要求这里拥有 payload
- 后续可以直接复用 chunk buffer 的只读视图
- 比再次拷贝 `std::string` 更适合高并发写入路径

### `DurableFlushRequest`

描述 flush 边界。

当前模式：

- `kDataOnly`
- `kDataAndMetadata`

后续 Linux / Windows 分支可以据此选择更合适的 flush 策略。

### `PublishDurableFileRequest`

描述 staging 到 final 的 publish。

关键字段：

- `staging_path`
- `final_path`
- `mode`

`mode` 预留了：

- `kExclusive`
- `kReplaceExisting`

这样后续既能表达“目标必须不存在”，也能表达受控替换。

### `SyncDurableDirectoryRequest`

单独表达 directory sync。

原因是它和文件 flush 是两个不同的 durability 边界，后续 Windows 分支也可能只能给出 weaker contract 或 explicit unsupported。

## 主要接口

### `DurableFileWriter`

表示一次 staging 写入会话。

主要函数：

- `Append(...)`
- `Flush(...)`
- `Close(...)`
- `path()`

它只表达“写一个 staging 文件”的过程，不负责 publish 到 final。

### `DurableFile`

表示 durable file 抽象入口。

主要函数：

- `NormalizePath(...)`
- `OpenStagingWriter(...)`
- `PublishStagedFile(...)`
- `SyncDirectory(...)`

这样后续 `LocalDiskChunkStore` 可以把“写 staging”和“publish/sync”拆开控制，而不是把所有文件动作塞进一个黑盒函数里。

## 共享错误映射

本模块提供：

- `ToString(DurableFileErrorCode)`
- `MapDurableFileErrorCode(...)`
- `IsRetriableDurableFileError(...)`

用途：

- 文件层先保留更细粒度原因
- 上层再统一映射到 `StorageNodeStatusCode`

## 当前未实现内容

- 平台文件句柄
- 真实 append / flush / close
- 真实 atomic publish
- 真实 directory sync
- 真实路径规范化规则

这些留给：

- T012：契约测试骨架
- T013：Linux 分支
- T014：Windows 分支

## 测试边界

T012 的契约测试骨架只固定接口语义，不证明平台 durability 已实现。

当前测试重点是：

- required operation 不能用 silent no-op success 冒充成功
- `unsupported` 和显式错误必须对调用方可见
- flush / publish / directory sync 的结果要能区分“达到 durability 边界”和“没有达到边界”
