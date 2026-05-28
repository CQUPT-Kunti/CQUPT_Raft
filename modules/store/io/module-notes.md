# store/io 说明

## 模块职责

`modules/store/io` 定义 durable file 抽象接口和平台实现边界。

它负责：

- staging 文件写入
- flush / durability boundary
- staging 到 final 的 atomic publish
- parent directory sync
- 路径规范化
- 跨平台共享错误分类

它不负责：

- `LocalDiskChunkStore` 业务流程
- ChunkIndex 容器
- StorageNodeService / Placement / Repair / Rebalance
- 更上层 restart cleanup / rebuild 编排

## 文件对照

- `durable_file.h`：公开错误码、请求/响应、接口声明、`LinuxDurableFile` / `WindowsDurableFile` 声明
- `durable_file.cpp`：错误映射、Linux / Windows 实现、内部 writer

如果你想找“某个文档条目具体对应哪个 `.cpp` 函数”，主要看下面这节。

## 主要结构体和类

### `DurableFileErrorCode`

durable file 层自己的错误分类。

比 `StorageNodeStatusCode` 更细，重点补了这些文件层错误：

- `kPartialWrite`
- `kPathInvalid`
- `kAtomicPublishFailed`
- `kDirectorySyncFailed`

### `DurableOperationContext`

给后续 timeout / cancellation / backpressure 预留控制字段：

- `timeout_ms`
- `best_effort_cancel`

### `DurableFileResult`

durable file 层统一返回结构。

关键字段：

- `error`
- `error_detail`
- `retry_after_ms`
- `bytes_transferred`
- `durable_boundary_reached`
- `partial_progress`

### `NormalizeDurablePathRequest / Response`

用于路径规范化和 root 内校验。

### `OpenStagingWriterRequest`

用于打开 staging writer。

关键字段：

- `relative_path`
- `expected_size`

### `DurableAppendRequest`

当前用 `std::span<const std::byte>` 承载字节视图，避免不必要拷贝。

### `PublishDurableFileRequest`

描述 staging 到 final 的 publish。

关键字段：

- `staging_path`
- `final_path`
- `mode`

### `SyncDurableDirectoryRequest`

单独表达 directory sync，因为它和文件 flush 是两个不同的 durability 边界。

### `DurableFileWriter`

表示一次 staging 写入会话。

### `DurableFile`

表示 durable file 抽象入口。

### `LinuxDurableFile`

Linux 平台上的具体实现入口。

### `WindowsDurableFile`

Windows 平台上的具体实现入口。

## `.cpp` 里当前实现了哪些函数

下面这些公开函数都在 `durable_file.cpp` 里有实现：

### 共享错误映射

- `ToString(DurableFileErrorCode)`
- `MapDurableFileErrorCode(...)`
- `IsRetriableDurableFileError(...)`
- `DurableFileResult::status_code()`

### 抽象基类析构

- `DurableFileWriter::~DurableFileWriter()`
- `DurableFile::~DurableFile()`

### Linux durable file 入口

- `LinuxDurableFile::LinuxDurableFile(...)`
- `LinuxDurableFile::~LinuxDurableFile()`
- `LinuxDurableFile::NormalizePath(...)`
- `LinuxDurableFile::OpenStagingWriter(...)`
- `LinuxDurableFile::PublishStagedFile(...)`
- `LinuxDurableFile::SyncDirectory(...)`
- `LinuxDurableFile::root_path()`

### Windows durable file 入口

- `WindowsDurableFile::WindowsDurableFile(...)`
- `WindowsDurableFile::~WindowsDurableFile()`
- `WindowsDurableFile::NormalizePath(...)`
- `WindowsDurableFile::OpenStagingWriter(...)`
- `WindowsDurableFile::PublishStagedFile(...)`
- `WindowsDurableFile::SyncDirectory(...)`
- `WindowsDurableFile::root_path()`

## `durable_file.cpp` 里的内部实现

`durable_file.cpp` 里还有两个内部 writer：

- `LinuxDurableFileWriter`
- `WindowsDurableFileWriter`

它们都不在头文件暴露，分别负责真实平台写入路径：

- `Append(...)`：实际写入循环
- `Flush(...)`：调用平台 flush 能力
- `Close(...)`：关闭底层句柄
- `path()`

也就是说，`OpenStagingWriter(...)` 返回的 writer，真正的行为就在这个内部类里。

## 当前 Linux 语义

当前 Linux 分支已经提供：

- `write` 短写处理和 `EINTR` 重试
- `fdatasync` / `fsync`
- same-filesystem `rename`
- parent directory `fsync`
- root 内路径规范化和逃逸拒绝

这些语义的主要入口分别是：

- 路径校验：`LinuxDurableFile::NormalizePath(...)`
- 打开 writer：`LinuxDurableFile::OpenStagingWriter(...)`
- publish：`LinuxDurableFile::PublishStagedFile(...)`
- directory sync：`LinuxDurableFile::SyncDirectory(...)`

## 当前未实现内容

- 更上层的 `LocalDiskChunkStore` durable publish 流程
- 故障注入、恢复和并发写入编排

## 当前 Windows 语义

当前 Windows 分支已经提供：

- `WriteFile` 写入循环
- `FlushFileBuffers`
- `MoveFileExW` publish
- UTF-8 路径转 UTF-16
- long path 前缀处理
- reserved names / 非法字符 / 绝对路径 / `..` 拒绝

这些语义的主要入口分别是：

- 路径校验：`WindowsDurableFile::NormalizePath(...)`
- 打开 writer：`WindowsDurableFile::OpenStagingWriter(...)`
- publish：`WindowsDurableFile::PublishStagedFile(...)`
- directory durability：`WindowsDurableFile::SyncDirectory(...)`

当前 directory durability 还没有给出等价实现，所以 `WindowsDurableFile::SyncDirectory(...)` 会返回 explicit `kUnsupported`，而不是 no-op success。

## 测试边界

- T012 固定的是接口契约
- T013 之后，Linux 测试会额外验证真实的 flush / publish / directory sync 路径
- T014 之后，Windows 用例已经写好条件编译测试，但如果当前环境不是 Windows，它们不会在本机执行

但这些测试仍然只覆盖 durable file 模块本身，不等于已经完成上层 chunk store 的恢复语义证明。
