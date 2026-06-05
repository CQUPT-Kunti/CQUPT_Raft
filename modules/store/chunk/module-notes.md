# store/chunk 说明

## 模块职责

`modules/store/chunk` 定义 chunk data-plane 的抽象接口边界，以及本地磁盘 store 的入口骨架。

它当前只负责：

- `ChunkStore` 抽象接口
- chunk 请求/响应结构
- `LocalDiskChunkStore` 配置、初始化和 `WriteChunk` 写入入口
- `LocalDiskChunkStore::ReadChunk` 的最小真实读取路径
- `LocalDiskChunkStore::DeleteChunk` / `StatChunk` / `ListChunks` 的本地 index 语义
- 与 `store/common` 类型的拼装边界

它当前不负责：

- 完整 durable publish 编排
- ChunkIndex 的具体容器实现
- StorageNode RPC
- `CreateObject` / `CommitObject` / `AbortObject` 之类 metadata control-plane 生命周期决策
- repair / rebalance / GC 后台能力

## 命名空间

本模块统一使用 `storedemo`。

## 文件对照

- `chunk_store.h`：接口和 request / response 结构
- `chunk_store.cpp`：当前只有 `ChunkStore::~ChunkStore()`
- `local_disk_chunk_store.h`：`LocalDiskChunkStore` 配置、初始化结果和类声明
- `local_disk_chunk_store.cpp`：目录初始化、默认依赖装配，以及 `WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 的本地实现

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
- `quarantine_root`

它只表达当前已经准备好的目录边界，不承载索引或恢复状态。

### `LocalDiskChunkStoreConfig`

`LocalDiskChunkStore` 的最小配置。

关键字段：

- `data_dir`
- `node_id`
- `durable_file`
- `chunk_index`
- `executor`
- `staging_cleanup_grace_period_ms`

当前语义：

- `data_dir` 和 `node_id` 必须有效
- `durable_file` 允许为空；为空时会按当前平台创建默认 durable file 实现
- `chunk_index` 允许为空；为空时会创建默认 `ShardedChunkIndex`
- `executor` 当前只是后续异步路径的扩展点，初始化阶段允许为空
- `staging_cleanup_grace_period_ms` 控制 `chunks/staging/` 下 stale / partial staging 的清理阈值；默认 5 分钟，`0` 表示禁用 cleanup

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
- 初始化 `data_root`、`chunks/`、`chunks/live/`、`chunks/staging/`、`chunks/quarantine/`
- 默认 durable file / chunk index 依赖装配
- `RebuildIndexFromDisk`
- `Initialize()` 自动执行 live/quarantine chunk index rebuild

T023-T025 已实现：

- `WriteChunk`
- `ReadChunk`
- `DeleteChunk`
- `StatChunk`
- `ListChunks`

后续任务仍未实现：

- 后台 scrub / repair / rebalance

其中：

- `WriteChunk` 已经走通 staging -> flush -> publish -> directory sync -> LIVE index update
- `ReadChunk` 已经走通 index lookup -> state check -> final file read -> checksum verify；发现 size/checksum 与 index metadata 不一致时会把本地 final chunk 移入 quarantine，并将 index 状态更新为 `kQuarantined`
- `DeleteChunk` 已经走通 chunk guard -> expected checksum 校验 -> final file remove -> index state update
- `StatChunk` 已经走通 index lookup 和可选 checksum verify；verify 阶段发现 size/checksum 不一致时也会触发 quarantine
- `ListChunks` 已经走通基于 `ChunkIndex` 的过滤和分页
- `RebuildIndexFromDisk` 已经走通 canonical `chunks/live/` + `chunks/quarantine/` 目录扫描、payload size/checksum 恢复，以及 `LIVE` / `QUARANTINED` index 重建
- `Initialize()` 已经走通 staging scan -> stale staging cleanup -> live/quarantine index rebuild
- T026 已用真实本地文件压力测试覆盖不同 chunk 并发写入、同 chunk 冲突写入，以及读/删/查/列交错边界

当前并发边界补充：

- 不同 chunk 的 `WriteChunk` 可以并行推进
- 同一个 chunk 的并发写入只允许一个 payload 胜出；同内容后续请求幂等成功，不同内容返回 `kConflict`
- `ReadChunk` / `ListChunks` 当前不持有 chunk guard，也不提供并发分页快照语义；因此读删交错时允许出现 `kOk`、`kNotFound` 或显式 `kIoError`
- T027 的上传闭环测试已固定当前集成边界：`LocalDiskChunkStore` 只负责 chunk durable write/read，何时调用 metadata `CommitObject` 必须由上层 coordinator / service orchestration 决定；store 本身不会也不应该直接提交 metadata
- T028 的 `WriteChunk` contract 测试已固定未来 service/client 适配层的最小兼容语义：成功只表示 chunk 已 durable，不表示 metadata 已 committed；同内容重复写允许以 success/`already_exists` 返回；不同内容冲突必须显式返回；executor admission 满时要映射为 `kOverloaded`
- `StorageTaskContext.timeout_ms` / `best_effort_cancel` 在当前 runtime 里仍只是扩展字段；适配层不能把它们包装成“已经具备运行中取消传播”的既成事实

## `.cpp` 当前实现了什么

- `ChunkStore::~ChunkStore()`
- `LocalDiskChunkStore::LocalDiskChunkStore(...)`
- `LocalDiskChunkStore::~LocalDiskChunkStore()`
- `LocalDiskChunkStorePaths::IsInitialized()`
- `LocalDiskChunkStore::Initialize()`
- `LocalDiskChunkStore::RebuildIndexFromDisk()`
- `LocalDiskChunkStore::config()`
- `LocalDiskChunkStore::paths()`
- `LocalDiskChunkStore::initialized()`
- `LocalDiskChunkStore::durable_file()`
- `LocalDiskChunkStore::chunk_index()`
- `LocalDiskChunkStore::executor()`
- `LocalDiskChunkStore::WriteChunk()`
- `LocalDiskChunkStore::ReadChunk()`
- `LocalDiskChunkStore::DeleteChunk()`
- `LocalDiskChunkStore::StatChunk()`
- `LocalDiskChunkStore::ListChunks()`

也就是说：

- `chunk_store.cpp` 的职责仍然只是提供虚析构定义，避免接口类链接缺口
- `local_disk_chunk_store.cpp` 现在已经包含 `Initialize` / `RebuildIndexFromDisk` / `WriteChunk` / `ReadChunk` / `DeleteChunk` / `StatChunk` / `ListChunks` 的本地路径
- restart live/quarantine index rebuild、stale staging cleanup 和 read/stat quarantine 已实现，但后台 scrub/repair/rebalance 仍未实现

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
- `kQuarantineDirectoryRelativePath`
  - 表示 `chunks/quarantine`

这四个常量只用于 `Initialize()` 里生成目录布局，避免把相对路径字面量散落在实现里。

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

### `CollectRegularFileCandidatePaths(...)`

递归扫描指定根目录下的 regular file 候选，并返回相对 `data_root` 的稳定排序结果。

输入：

- `data_root`
- `scan_root`
- `root_label`

输出：

- 所有 regular candidate 的相对路径列表

边界：

- 只负责目录遍历和 regular file 过滤
- root 不存在、不是目录或遍历失败都返回明确错误
- 被 `CollectLiveChunkCandidatePaths(...)` 和 `CollectQuarantineChunkCandidatePaths(...)` 复用

### `CollectLiveChunkCandidatePaths(...)`

递归扫描 `chunks/live/` 下的 regular file 候选，并返回相对 `data_root` 的稳定排序结果。

输入：

- `data_root`
- `live_root`

输出：

- 所有 regular candidate 的相对路径列表

边界：

- 只负责 live 目录遍历
- 非 regular 候选直接跳过
- 目录不存在或遍历失败返回明确文件系统错误

### `CollectQuarantineChunkCandidatePaths(...)`

递归扫描 `chunks/quarantine/` 下的 regular file 候选，并返回相对 `data_root` 的稳定排序结果。

输入：

- `data_root`
- `quarantine_root`

输出：

- quarantine candidate 的相对路径列表

边界：

- 只负责 quarantine 目录遍历
- 不把 quarantine 候选直接当成 live

### `CollectStagingCleanupCandidates(...)`

递归扫描 `chunks/staging/`，收集 staging file candidate 和待 prune 的目录列表，并固定稳定顺序。

输入：

- `staging_root`

输出：

- `file_candidates`
- `directory_candidates`

边界：

- 只扫描 staging 目录
- regular file 进入 cleanup 候选
- 目录按“更深优先、同层字典序”排序，便于稳定 prune

### `IsStagingCandidatePastGracePeriod(...)`

基于 `last_write_time` 和 `staging_cleanup_grace_period_ms` 判断 staging 候选是否已经 stale。

输入：

- `path`
- `grace_period_ms`

输出：

- `is_stale`

边界：

- `0` 表示禁用 cleanup
- mtime 在未来时按 fresh 处理
- mtime 读取失败返回明确文件系统错误

### `RemovePathSafelyUnderRoot(...)`

在确认目标仍位于 `staging_root` 之下后，删除单个 staging file 或空目录。

输入：

- `root_path`
- `target_path`

输出：

- remove 结果状态

边界：

- 拒绝删除 root 本身或 root 外路径
- 不存在视为幂等成功
- 删除失败返回明确错误，不 silent success

### `CleanupStaleStagingFiles(...)`

遍历 staging file candidate，对超过 grace period 的 stale / partial staging 执行删除。

输入：

- `staging_root`
- `grace_period_ms`
- `scan_result`

输出：

- 文件 cleanup 结果状态

边界：

- fresh staging 不删除
- malformed staging 文件只要在 staging 下且超过阈值，也按 staging 垃圾处理
- cleanup 失败会中止初始化

### `PruneEmptyStagingDirectories(...)`

在 stale file 删除后，按稳定顺序清理已经变空的 staging 子目录。

输入：

- `staging_root`
- `scan_result`

输出：

- 目录 prune 结果状态

边界：

- 只删除空目录
- 非空目录保留，避免误删 fresh staging
- 删除失败返回明确错误

### `CleanupStaleStagingArtifacts(...)`

这是 T071 的 recovery cleanup 主流程。

主步骤：

1. 扫描 `chunks/staging/`
2. 用 grace period 判断 stale staging
3. 删除 stale / partial staging file
4. prune 删除后留下的空 staging 目录

边界：

- 不扫描或删除 `chunks/live/`
- 不把 staging 事实加入 live index
- cleanup 失败返回明确错误
- 不调用 metadata / Raft

### `ParseChunkIdFromLiveFilename(...)`

从 live final 文件名中提取 `chunk_id`，并复用 `ValidateChunkId(...)` 做合法性校验。

输入：

- `relative_path`

输出：

- 合法 `chunk_id`

边界：

- malformed / invalid filename 返回非 `kOk`
- T070 当前调用方把这类候选安全跳过，不升级成 quarantine

### `IsCanonicalLiveChunkPath(...)`

基于 `BuildChunkPathLayout(...)` 判断某个 live candidate 是否位于 canonical final shard 路径。

输入：

- `chunk_id`
- `relative_path`

输出：

- `is_canonical`

边界：

- 只接受 `chunks/live/<shard>/<shard>/<chunk_id>.chunk`
- misplaced live candidate 不进入 rebuilt index

### `BuildQuarantineChunkRelativePath(...)`

根据 `chunk_id` 生成 canonical quarantine 相对路径。

输入：

- `chunk_id`

输出：

- `chunks/quarantine/<shard>/<shard>/<chunk_id>.chunk`

边界：

- 复用 `BuildChunkPathLayout(...)` 的 shard 规则
- 不引入新的 chunk 命名格式

### `IsCanonicalQuarantineChunkPath(...)`

判断 quarantine candidate 是否位于 canonical quarantine shard 路径。

输入：

- `chunk_id`
- `relative_path`

输出：

- `is_canonical`

边界：

- misplaced quarantine candidate 不进入 rebuilt index

### `RecoverFinalChunkPayloadFacts(...)`

读取 final chunk payload，并恢复当前磁盘上可直接验证的 `size` 和 `checksum`。

输入：

- `final_path`

输出：

- `size`
- `checksum`

边界：

- zero-byte final file 合法
- 只基于文件内容恢复事实
- 不会凭空推断“此前未标记”的 checksum mismatch；更强 corruption 判定仍依赖本地 quarantine 目录事实或前台读/查时检测

### `BuildRebuiltChunkIndexEntry(...)`

把恢复出的 chunk identity、size、checksum 和 final path 组装成 `ChunkIndexEntry`。

输入：

- `identity`
- `size`
- `checksum`
- `state`
- `final_relative_path`

输出：

- `ChunkIndexEntry`

边界：

- 当前可重建 `ChunkState::kLive` 和 `ChunkState::kQuarantined`
- `updated_at` 当前不伪造历史时间，保持恢复态默认值

### `QuarantineChunkEntry(...)`

这是 T072 的核心隔离 helper。

主步骤：

1. 对目标 `chunk_id` 获取 per-chunk guard
2. 重新读取最新 `ChunkIndexEntry`
3. 把 canonical live final chunk 移到 canonical quarantine 路径
4. 将 index 状态更新为 `kQuarantined`

输入：

- `paths`
- `chunk_index`
- `chunk_id`

输出：

- 更新后的 quarantined entry

边界：

- 只处理当前仍是 `kLive` 的 entry
- quarantine target 已存在、rename 失败或路径异常都会返回明确错误
- 不调用 metadata / Raft
- 不做 repair，不删除 payload

### `ClearChunkIndexEntries(...)`

在重建前清空现有 `ChunkIndex`，确保重启恢复只依赖本地磁盘事实，而不是信任旧内存索引。

输入：

- `chunk_index`

输出：

- 清空结果状态

边界：

- 通过 `List` + `Remove` 清理已有 entry
- 失败时返回明确索引错误，不静默保留旧事实

### `InsertRecoveredChunkIndexEntry(...)`

把恢复出的 entry 写回 `ChunkIndex`。

输入：

- `chunk_index`
- `entry`

输出：

- 插入结果状态

边界：

- duplicate / insert failure 返回明确错误
- 不在这里做 metadata / Raft 交互

### `CreateDefaultDurableFile(...)`

当调用方没有注入 `durable_file` 时，按当前平台创建默认 durable file 实现：

- Linux -> `LinuxDurableFile`
- Windows -> `WindowsDurableFile`
- 其它平台 -> 返回空指针

所以它负责的是“默认依赖装配”，不是执行 flush / publish。

### `MakeUnsupportedResponse<Response>(...)`

给当前尚未实现的接口生成统一返回。

当前语义是：

- `status = kUnsupported`
- `error_detail` 指明这是当前阶段尚未实现的接口

这样做的目的，是在阶段化实现过程中明确拒绝未实现能力，而不是 silent success。

### `RebuildIndexFromDisk()`

这是 T070 的主恢复流程。

主步骤：

1. 扫描 `chunks/live/` 和 `chunks/quarantine/`
2. 过滤非 `.chunk`、非法 `chunk_id`、misplaced candidate
3. 对同一 `chunk_id` 的多个 rebuild candidate 直接返回 `kConflict`
4. 读取 canonical final/quarantine chunk payload，恢复 `size` / `checksum` / `ChunkIdentity`
5. 清空现有 `ChunkIndex`
6. 以 `ChunkState::kLive` 或 `ChunkState::kQuarantined` 重建本地 index

边界：

- 不扫描 staging 进入 live index
- 不能仅靠 `chunks/live/*.chunk` payload bytes 自行发现“此前未标记”的 checksum mismatch
- 不调用 metadata / Raft
- 不决定 object committed/deleted 可见性

### `CurrentUnixTimeMillis()`

给 `WriteChunk` 生成本地 metadata / index 更新时间。

### `BuildHexToken(...)`

把 `request_id` 映射成安全的 staging token。

当前 token 只要求：

- 单路径段安全
- 同 chunk 重试可复现
- 不把原始 request_id 直接暴露成文件名

### `HasExpectedChecksumConstraint(...)`

判断请求是否真的带了 expected checksum 约束。

它的作用是区分：

- 完全未设置 expected checksum
- 部分设置但格式不合法的 expected checksum

### `BuildChunkMetadata(...)`

把一次成功写入的结果组装成 `ChunkMetadata`。

### `BuildChunkMetadataFromIndexEntry(...)`

把已有 index 条目转换成返回给调用方的 `ChunkMetadata`。

这个 helper 主要给重复写的 idempotent 成功路径复用。

### `BuildChunkIndexEntry(...)`

把成功写入后的 metadata 和 final path 组装成 `ChunkIndexEntry`。

### `ResolveIndexedFinalPath(...)`

给 `ReadChunk` / `DeleteChunk` / `StatChunk` 解析 final 文件路径。

优先级是：

- 先使用 index entry 里已经记录的 `final_path`
- 如果 entry 里还没有 final path，再基于 `chunk_id` 反推出标准 final layout

它不会回退去读 staging。

### `CompareChecksums(...)`

比较两份已经成形的 checksum。

这个 helper 主要给：

- `ReadChunk` 的 index checksum 对比
- `DeleteChunk` 的 `expected_checksum` 保护
- `StatChunk(verify_checksum=true)` 的核验

### `ResolveEntryChecksum(...)`

解析一个 index entry 当前应该用于校验的 checksum。

优先级是：

- 先用 entry 自己记录的 checksum
- 如果 entry 没有 checksum，再退到 final 文件内容计算

### `ValidateDeleteExpectedChecksum(...)`

处理 `DeleteChunkRequest.expected_checksum`。

它保证：

- 如果 caller 给了 expected checksum，删除前必须校验
- mismatch 时直接返回错误
- mismatch 不会误删文件，也不会错误把 index 改成 `DELETED`

### `ValidateReadableChunkState(...)`

把 `ChunkState` 映射成读路径是否允许继续。

当前语义：

- `LIVE` 才允许读
- `CORRUPTED` / `QUARANTINED` 返回明确损坏错误
- `DELETED` / `MISSING` 返回 `kNotFound`
- `STAGING` / `DELETING` 返回冲突类错误

### `ValidateExpectedReadChecksum(...)`

处理 `ReadChunkRequest.expected_checksum` 的形状校验和结果比对。

它负责区分：

- checksum 结构本身非法
- 算法不支持
- 期望 checksum 与实际 payload 不一致

### `ReadFilePayload(...)`

按二进制方式读取 final chunk 文件。

它只负责文件读取和 IO 错误分类，不做状态或 checksum 判断。

### `PrepareWriteIdentity(...)`

统一整理和校验 `WriteChunkRequest.identity`。

当前支持两种输入方式：

- 请求直接带合法 `chunk_id`
- 请求没带 `chunk_id`，但带了 `object_id + version + chunk_index`

如果请求里同时带了 `chunk_id` 和诊断字段，它会检查是否一致。

### `HasRequiredDurableBoundary(...)`

校验 durable file 的 required operation 是否真的到达 durable boundary。

这层检查会拦住“返回 `kOk` 但其实没有 durable boundary”的 silent success。

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
7. 扫描并 cleanup stale staging / partial staging
8. 重建 live ChunkIndex
9. 写回 `initialized_`

也就是说，`Initialize()` 现在除了目录落稳，还会先做 recovery cleanup，再做 live index rebuild。

## `WriteChunk()` 当前语义

### 写入顺序

当前 `WriteChunk()` 的主流程是：

1. 必要时触发 `Initialize()`
2. 校验 `request_id`
3. 校验并整理 `ChunkIdentity`
4. 校验 `expected_size`
5. 计算或校验 checksum
6. 获取 `ChunkIndex` 的 per-chunk guard
7. 检查现有 index 条目，处理重复写 / 冲突
8. 生成 final / staging 路径
9. 打开 staging writer
10. 写入 payload
11. flush staging
12. close writer
13. publish 到 final
14. sync final parent directory
15. insert LIVE index

只有第 15 步成功之后，chunk 才会进入 LIVE index。

### 幂等与冲突

当前重复写语义：

- same chunk + same size + same checksum -> 返回成功，并设置 `already_exists=true`
- same chunk + 不同 size 或 checksum -> 返回 `kConflict`
- payload 和 `expected_checksum` 自己不一致 -> 返回 `kChecksumMismatch`

当前没有更强的“内容 identity”字段，所以重复写判断以 `size + checksum` 为主。

### chunk guard 边界

`WriteChunk()` 当前已经把 `AcquireChunkLock()` 作为同 chunk 写入主入口的固定前置步骤。

这意味着：

- 同一 chunk 的冲突写入会串行
- 不同 chunk 仍然可以走不同 stripe 并行

### 当前未解决的恢复边界

T070 之后仍然没有解决这些恢复问题：

- corrupted / quarantine
- published final file 的 tombstone / deleted / metadata freshness 判定

也就是说，当前 `WriteChunk()` + `Initialize()`/`RebuildIndexFromDisk()` 已经保证 stale staging cleanup 和 live final chunk 的本地重启可发现，但还没有把坏块隔离或 metadata 事实新鲜度一起收口。

## `ReadChunk()` 当前语义

### 读取顺序

当前 `ReadChunk()` 的主流程是：

1. 必要时触发 `Initialize()`
2. 校验 `request_id`
3. 拒绝 range read
4. 从 `ChunkIndex` 查 chunk
5. 检查 state 是否为 `LIVE`
6. 解析 final path
7. 确认 final 文件存在
8. 读取整个文件 payload
9. 校验读取大小与 index metadata 一致
10. 计算实际 checksum
11. 校验实际 checksum 与 index checksum 一致
12. 如果请求带了 `expected_checksum`，继续校验请求期望
13. 仅在全部通过后返回 payload

### 当前边界

- 只支持 full read，不支持 range read
- 只读取 final 文件，不回退 staging
- 非 `LIVE` 状态不会返回成功
- 检测到文件大小或 checksum 与 index metadata 不一致时，当前返回明确错误，但**不会在 T024 自动把 index 状态改成 `CORRUPTED`**

最后这一点是有意保持收敛范围：T024 先固定读路径校验语义，不在这一轮把损坏状态写回、后台隔离或恢复流程一起做掉。

## `DeleteChunk()` 当前语义

### 删除顺序

当前 `DeleteChunk()` 的主流程是：

1. 必要时触发 `Initialize()`
2. 校验 `request_id`
3. 获取 `ChunkIndex` 的 per-chunk guard
4. 从 index 查 chunk
5. 如果 chunk 缺失，返回幂等成功并标记 `already_missing=true`
6. 如果请求带 `expected_checksum`，先校验当前 metadata / 文件 checksum
7. 只删除 final 文件，不删除 staging 文件
8. 删除成功后把 index state 更新为 `DELETED`

### 当前删除状态语义

- 当前实现选择**保留 `DELETED` 条目在内存 index 中**
- 这样本进程生命周期内：
  - repeated delete 可以幂等返回
  - `StatChunk` 可以看到 `DELETED`
  - `ListChunks` 可以按 `DELETED` 过滤

这不是持久 tombstone；重启后是否还能重建出来，仍取决于后续 restart rebuild 任务。

### `expected_checksum` 保护

如果请求带了 `expected_checksum`：

- 优先使用 index 中已有 checksum 做校验
- 如果 index 没有 checksum，再退到 final 文件内容计算
- mismatch 时直接返回错误，不删除文件，也不把 index 错误改成 `DELETED`

## `StatChunk()` 当前语义

- 只查 `ChunkIndex`
- chunk 不存在返回 `kNotFound`
- 默认不扫描文件系统
- `verify_checksum=true` 时，仅对 `LIVE` chunk 做 final 文件读取和 checksum 校验
- 当前发现损坏时会返回明确错误，但不会自动把 index 状态回写成 `CORRUPTED`

## `ListChunks()` 当前语义

- 只通过 `ChunkIndex` 列举，不扫描文件系统
- 支持 `state_filter`
- 支持 `page_token` + `page_size`
- 默认依赖 `ChunkIndex` 的分页边界，不在 T025 引入更强的并发快照语义
- 不会因为磁盘上存在未登记 final 文件就把它列出来

## 与其它模块的边界

- 复用 `store/common/store_types.h` 里的 `ChunkId`、`ChunkIdentity`、`ChunkMetadata`、`ChunkChecksum`、`ChunkState`、`StorageNodeStatusCode`
- 后续 `modules/store/io` 提供 durable file 能力
- `LocalDiskChunkStore` 已开始复用 `modules/store/io` 的路径解析 helper，并接入 `DurableFile`
- `LocalDiskChunkStore` 也会复用 `modules/store/index` 的 `ChunkIndex`
- 后续 `StorageNodeService` 只负责把 RPC 映射到 `ChunkStore`

## 当前未实现内容

- 读路径发现损坏后的自动状态回写
- quarantine / repair / GC / restart recovery
- range read
