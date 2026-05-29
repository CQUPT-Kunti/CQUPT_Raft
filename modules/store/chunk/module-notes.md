# store/chunk 说明

## 模块职责

`modules/store/chunk` 定义 chunk data-plane 的抽象接口边界，以及本地磁盘 store 的入口骨架。

它当前只负责：

- `ChunkStore` 抽象接口
- chunk 请求/响应结构
- `LocalDiskChunkStore` 配置和初始化入口
- 与 `store/common` 类型的拼装边界

它当前不负责：

- 真实 chunk payload 读写
- 完整 durable publish 编排
- ChunkIndex 的具体容器实现
- StorageNode RPC
- repair / rebalance / GC 后台能力

## 命名空间

本模块统一使用 `storedemo`。

## 文件对照

- `chunk_store.h`：接口和 request / response 结构
- `chunk_store.cpp`：当前只有 `ChunkStore::~ChunkStore()`
- `local_disk_chunk_store.h`：`LocalDiskChunkStore` 配置、初始化结果和类声明
- `local_disk_chunk_store.cpp`：目录初始化、默认依赖装配和当前未实现接口的显式返回

`chunk_store.cpp` 现在本来就以虚析构定义为主；真正开始承接本地实现后，新增的逻辑主要会进入 `local_disk_chunk_store.cpp`。

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

### `LocalDiskChunkStorePaths`

保存初始化后的目录落点：

- `data_root`
- `chunks_root`
- `live_root`
- `staging_root`

它只表达当前已经准备好的目录边界，不承载索引或恢复状态。

### `LocalDiskChunkStoreConfig`

`LocalDiskChunkStore` 的最小配置。

关键字段：

- `data_dir`
- `node_id`
- `durable_file`
- `chunk_index`
- `executor`

当前语义：

- `data_dir` 和 `node_id` 必须有效
- `durable_file` 允许为空；为空时会按当前平台创建默认 durable file 实现
- `chunk_index` 允许为空；为空时会创建默认 `ShardedChunkIndex`
- `executor` 当前只是后续异步路径的扩展点，初始化阶段允许为空

如果调用方自己注入 `durable_file`，它的 root 必须和 `data_dir` 对齐；当前骨架不会替调用方重写外部 durable file 的 root。

### `LocalDiskChunkStoreInitResult`

表达构造后的目录初始化结果。

关键字段：

- `status`
- `error_detail`
- `paths`
- `initialized`

### `LocalDiskChunkStore`

当前是 `ChunkStore` 的最小本地实现骨架。

T021 已实现：

- 配置持有
- 安全解析 `data_dir`
- 初始化 `data_root`、`chunks/`、`chunks/live/`、`chunks/staging/`
- 默认 durable file / chunk index 依赖装配

T021 还没有实现：

- `WriteChunk`
- `ReadChunk`
- `DeleteChunk`
- `StatChunk`
- `ListChunks`

这些接口当前会返回明确的 `kUnsupported`，不会伪装成功。

## `.cpp` 当前实现了什么

- `ChunkStore::~ChunkStore()`
- `LocalDiskChunkStore::LocalDiskChunkStore(...)`
- `LocalDiskChunkStore::~LocalDiskChunkStore()`
- `LocalDiskChunkStorePaths::IsInitialized()`
- `LocalDiskChunkStore::Initialize()`
- `LocalDiskChunkStore::config()`
- `LocalDiskChunkStore::paths()`
- `LocalDiskChunkStore::initialized()`
- `LocalDiskChunkStore::durable_file()`
- `LocalDiskChunkStore::chunk_index()`
- `LocalDiskChunkStore::executor()`
- `LocalDiskChunkStore::{WriteChunk, ReadChunk, DeleteChunk, StatChunk, ListChunks}()`

也就是说：

- `chunk_store.cpp` 的职责仍然只是提供虚析构定义，避免接口类链接缺口
- `local_disk_chunk_store.cpp` 当前只负责配置/目录初始化骨架，不做真实 chunk 数据路径

## `local_disk_chunk_store.cpp` 内部 helper 对照

`local_disk_chunk_store.cpp` 里有一组匿名 namespace helper。

它们不会暴露到头文件，所以如果只看 `.h`，确实会不知道这些函数在做什么。这里补一个一一对应的说明：

### 路径常量

- `kChunksDirectoryRelativePath`
  - 表示 `chunks`
- `kLiveDirectoryRelativePath`
  - 表示 `chunks/live`
- `kStagingDirectoryRelativePath`
  - 表示 `chunks/staging`

这三个常量只用于 `Initialize()` 里生成目录布局，避免把相对路径字面量散落在实现里。

### `MapFilesystemErrorToStatus(...)`

把 `std::filesystem` / `std::error_code` 错误归类到共享的 `StorageNodeStatusCode`。

当前主要映射：

- 磁盘空间不足 -> `kDiskFull`
- 权限/只读文件系统 -> `kPermissionDenied`
- 非法路径/路径不存在/不是目录/名字过长 -> `kInvalidArgument`
- 其它文件系统错误 -> `kIoError`

`Initialize()` 里遇到目录存在性检查、绝对路径解析、目录创建失败时，都会依赖这个函数统一错误分类。

### `BuildFilesystemErrorDetail(...)`

把文件系统错误拼成统一的文本细节：

- 操作名
- 目标路径
- 系统错误消息

它只是给 `error_detail` 生成可读字符串，不负责状态码映射。

### `EnsureDirectoryExists(...)`

保证某个目录已经存在，并且真的是目录。

它会做三件事：

- 先看路径是否已经存在
- 如果存在，确认它不是普通文件
- 如果不存在，就递归创建目录

这是 `Initialize()` 真正执行目录初始化的核心 helper。

### `ResolveStorePath(...)`

这是对 `ResolveDurablePathUnderRoot(...)` 的轻量包装。

它的作用不是重新实现路径校验，而是把 `chunk` 模块里“目录必须限制在 data root 内”的意图写得更直接一点。当前 `Initialize()` 用它把：

- `chunks`
- `chunks/live`
- `chunks/staging`

解析成 data root 下的安全绝对路径。

### `CreateDefaultDurableFile(...)`

当调用方没有注入 `durable_file` 时，按当前平台创建默认 durable file 实现：

- Linux -> `LinuxDurableFile`
- Windows -> `WindowsDurableFile`
- 其它平台 -> 返回空指针

所以它负责的是“默认依赖装配”，不是执行 flush / publish。

### `MakeUnsupportedResponse<Response>(...)`

给当前尚未实现的 `WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 生成统一返回。

当前语义是：

- `status = kUnsupported`
- `error_detail` 指明这是 T021 尚未实现的接口

这样做的目的，是在骨架阶段明确拒绝未实现能力，而不是 silent success。

## `Initialize()` 的执行顺序

如果你想快速理解 `LocalDiskChunkStore::Initialize()`，可以按下面顺序读：

1. 校验 `node_id`
2. 校验 `data_dir`
3. 把 `data_dir` 转成规范化绝对路径
4. 基于 data root 解析：
   - `chunks/`
   - `chunks/live/`
   - `chunks/staging/`
5. 如果调用方没注入依赖：
   - 创建默认 `ShardedChunkIndex`
   - 创建默认平台 `DurableFile`
6. 依次确保目录存在：
   - `data_root`
   - `chunks_root`
   - `live_root`
   - `staging_root`
7. 写回 `paths_` 和 `initialized_`

也就是说，`Initialize()` 当前只做“配置和目录边界落稳”，还没有进入后续的 chunk 生命周期逻辑。

## 与其它模块的边界

- 复用 `store/common/store_types.h` 里的 `ChunkId`、`ChunkIdentity`、`ChunkMetadata`、`ChunkChecksum`、`ChunkState`、`StorageNodeStatusCode`
- 后续 `modules/store/io` 提供 durable file 能力
- `LocalDiskChunkStore` 已开始复用 `modules/store/io` 的路径解析 helper，并接入 `DurableFile`
- `LocalDiskChunkStore` 也会复用 `modules/store/index` 的 `ChunkIndex`
- 后续 `StorageNodeService` 只负责把 RPC 映射到 `ChunkStore`

## 当前未实现内容

- 真实 chunk 文件写入 / 读取 / 删除
- flush / publish / directory sync 的上层编排
- checksum on write / on read 的实际流程串接
- quarantine / repair / GC / restart recovery
