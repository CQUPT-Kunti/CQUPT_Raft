# transfer 模块说明

## 模块职责

`modules/store/transfer/` 负责 `storage_client` 侧真实文件上传/下载的传输编排。它连接 ViewNode 服务发现、MetadataNode metadata control-plane 和 StorageNode data-plane，但不成为新的对象元数据权威，也不把真实 payload 带入 Raft。

当前规划中，本模块负责：

- 上传时按 bounded chunk 从文件读取数据，维护 chunk checksum 和对象级 checksum。
- 通过 ViewNode 发现 MetadataNode，并处理 leader hint / `NOT_LEADER` 传输边界。
- 调用 MetadataNode 获取 `WritePlan`。
- 按 `WritePlan` 将 chunk 写入 StorageNode data-plane。
- 收集 StorageNode 返回的 chunk write result，为 `CommitObject` 准备 chunk manifest facts。
- 下载时通过 MetadataNode 获取 COMMITTED manifest。
- 按 manifest 从 StorageNode 读取 chunk，逐 chunk 校验 checksum，拼接输出文件，并最终校验对象 checksum。
- 将 discovery failure、metadata RPC failure、StorageNode IO failure、checksum mismatch、commit failure 等错误转成清晰的客户端传输结果。

## 核心概念

### `ObjectTransfer`

- 面向 `storage_client` 的上传/下载编排入口。
- 负责把 discovery、metadata plan / manifest、StorageNode chunk read / write、checksum 和输出文件边界串起来。
- 不直接保存 object manifest 的权威副本，不决定对象可见性。

### `TransferSession`

- 单次 upload / download 的客户端执行上下文。
- 可记录 `request_id`、bucket、object key、object_id、version、chunk size、并发度、已完成字节数、checksum state 和失败摘要。
- 这是内存中的传输状态，不是 Raft 持久状态，也不是 StorageNode 本地状态。

### `MetadataTransferClient`

- transfer 模块访问 MetadataNode 的 adapter。
- 负责 `CreateWritePlan`、`CommitObject`、`HeadObject` / `GetObjectManifest` 等 metadata RPC 边界。
- 必须处理 `NOT_LEADER`、leader hint、超时、重试边界和幂等诊断。
- 不在本地判断 PENDING 是否可见；普通下载只接受 MetadataNode 返回的 COMMITTED manifest。

当前 `metadata_transfer_client.h` 只定义 adapter 接口、请求/结果类型、timeout / wait-for-ready 选项和 transport / metadata 诊断边界；不实现 RPC 调用逻辑。后续 `metadata_transfer_client.cpp` 负责把这些逻辑接口映射到 `MetadataService`，但仍不得在 adapter 层保存 object manifest 权威副本、判断对象最终可见性或实现 upload/download 编排。
T032 当前实现采用“现有 MetadataService 边界内的最小映射”：

- `CreateWritePlan` 通过现有 `CreateObject` RPC 创建 pending metadata 记录，并返回 transfer 可消费的对象 identity / checksum facts。
- 当前 `metadata.proto` 还没有显式返回 chunk layout / placement，因此 `TransferWritePlan.chunks` 暂不由 adapter 伪造。
- `CommitObject` 通过现有 `CommitObject` RPC 提交 chunk manifest facts，不提交 payload。
- `GetObjectManifest` 通过现有 `HeadObject` RPC 读取 COMMITTED `ObjectRecord` 并转换为 transfer manifest facts。
- 由于当前 `HeadObject` 只暴露 COMMITTED 对象，adapter 不能从现有 service 中区分“对象真实不存在”和“PENDING 但对普通读不可见”的全部细粒度状态。

### `StorageTransferClient`

- transfer 模块访问 StorageNode 的 data-plane adapter。
- 负责 `WriteChunk`、`ReadChunk` 和必要的 chunk endpoint 解析。
- 必须通过 StorageNode contract 交互，不直接修改 StorageNode 本地 chunk 文件、索引或 publish 状态。
- checksum mismatch、durability failure、IO failure 必须显式返回失败。
- T033 头文件边界：
  - `StorageTransferTarget`：封装 transfer 已解析出的 `node_id` + `endpoint`
  - `StorageTransferWriteRequest/Result`：表达单节点 chunk 写入、幂等重试 request_id、durable / already_exists / retryable 诊断
  - `StorageTransferReadRequest/Result`：表达单节点 chunk 读取、expected checksum、校验结果和 bounded payload 返回
  - `StorageTransferClient`：只负责单次 `WriteChunk` / `ReadChunk` adapter，不负责 manifest 选择、对象可见性、commit 或整文件编排
- T034 实现边界：
  - `storage_transfer_client.cpp` 当前复用 `StorageNodeClient` 发起单节点 `WriteChunk` / `ReadChunk`
  - 可以做 endpoint -> gRPC channel 的轻量缓存，但不缓存对象可见性或 manifest authority
  - 只转换 data-plane request/response、retryable 状态、durable/already_exists/verified 等事实
  - 不把 StorageNode 本地 live chunk 状态解释成对象 `COMMITTED` 可见

### chunk reader

- 从源文件按配置 chunk size 分段读取。
- 每次只持有 bounded chunk buffer，不允许把完整文件读入常驻内存。
- 应能提供 chunk index、offset、expected size、chunk checksum 和对象 checksum 的增量输入。

### checksum state

- 维护 chunk 级 checksum 和对象级 checksum 的传输侧状态。
- 上传时用于向 WritePlan / WriteChunk / CommitObject 提供可验证事实。
- 下载时用于逐 chunk 校验和最终对象 checksum 校验。
- 校验失败必须终止对应传输并报告，不得静默修正或继续提交成功。

### `object_transfer.h` 接口边界

`modules/store/transfer/object_transfer.h` 只定义 transfer 编排层的接口边界，不实现真实上传下载。当前头文件应表达：

- `ObjectTransfer` 作为 `storage_client` 侧 upload/download 编排入口
- `TransferSession` / `UploadTransferSession` / `DownloadTransferSession` 的生命周期快照边界
- `TransferChunkReader` 的 bounded chunk 读取接口，明确单次只返回单个 chunk buffer
- 默认 `CreateFileTransferChunkReader()` factory，用于本地 bounded 文件分块读取
- `TransferChecksumState` 的增量 checksum 边界，禁止依赖整文件常驻内存
- 默认 `CreateTransferChecksumState()` factory，用于维护对象级增量 checksum
- `TransferPreparedChunk`，表达 upload 本地读取后得到的 chunk index/offset/size/checksum facts
- `TransferWritePlan`、`TransferCommittedChunk`、`TransferCommittedManifest` 这些 metadata facts 边界
- 与 `MetadataTransferClient`、`StorageTransferClient`、`ViewNodeClient` 的依赖注入边界
- `UploadObjectRequest` / `DownloadObjectRequest` 中的 `cluster_id`，用于显式绑定 ViewNode discovery 范围，避免硬编码 demo 拓扑

它不负责：

- 实现 StorageNode `WriteChunk` / `ReadChunk`
- 实现 MetadataNode `CreateWritePlan` / `CommitObject`
- 实现 ViewNode discovery 重试循环
- 让 payload、chunk bytes 或完整文件进入 Raft metadata/control-plane

## 上传流程边界

1. `storage_client` 创建 upload `TransferSession`，设置源文件、bucket、object key、chunk size、request_id 和并发策略。
2. transfer 通过 ViewNode 发现 MetadataNode 候选地址；ViewNode 只提供发现和观测，不提供对象可见性依据。
3. transfer 调用 MetadataNode 获取 `WritePlan`。`WritePlan` 只包含 object metadata、chunk layout、placement、checksum、size、version 等 metadata，不包含真实 payload。
4. transfer 用 chunk reader 逐块读取源文件，按 `WritePlan` 调用 StorageNode `WriteChunk`。
5. StorageNode 返回 chunk_id、node_id、size、checksum、state、durable 等结果后，transfer 收集 chunk manifest facts。
6. 全部必要 chunk 写入满足 plan 后，transfer 调用 MetadataNode `CommitObject`。
7. 只有 MetadataNode 通过 Raft majority commit 使对象进入 COMMITTED 后，对象才对普通下载可见。

upload 失败时，transfer 可以产生清理候选或失败摘要，但不能自行把对象标记为 COMMITTED，也不能直接删除 StorageNode 本地状态绕过 StorageNode contract。

## 下载流程边界

1. `storage_client` 创建 download `TransferSession`，设置目标 bucket、object key、输出路径和 request_id。
2. transfer 通过 ViewNode 发现 MetadataNode 候选地址，并继续处理 `NOT_LEADER` / leader hint。
3. transfer 调用 MetadataNode 获取 COMMITTED manifest。PENDING、ABORTED、EXPIRED 或不可见对象不得由 transfer 自行解释为可下载对象。
4. transfer 按 manifest 中的 `ChunkManifest` 解析 chunk_id、chunk index、offset、size、checksum 和 replica node_id。
5. transfer 通过 StorageNode contract 读取 chunk，逐 chunk 校验 checksum，并按 offset / index 写入输出文件。
6. 所有 chunk 拼接完成后，transfer 校验最终对象 checksum / etag。
7. 任一 chunk checksum mismatch、对象 checksum mismatch、IO failure 或 manifest 与实际数据不一致，都必须显式失败。

下载路径不能依赖 ViewNode 的观测信息判断对象是否可见；ViewNode 最多用于发现 MetadataNode 或解析 StorageNode endpoint 候选。

## T035 discovery 集成边界

- transfer 可以用 ViewNode 的 `DiscoverMetadata` 选择 MetadataNode endpoint 候选，并用 leader hint 作为重试优化信息。
- 该 endpoint 只是候选地址，MetadataService 仍必须自己返回 `NOT_LEADER` / quorum / 可见性权威结果。
- transfer 可以用 `DiscoverStorage` 记录 StorageNode endpoint / 健康 / 容量观测，用于后续把 `WritePlan` 或 manifest 中的 node_id 解析到 data-plane endpoint。
- `DiscoverStorage` 返回的是观测事实，不是对象 manifest，不是对象可见性权威，也不能替代 MetadataNode 的 COMMITTED manifest。

## T036 下载重建边界

- download 只接受 MetadataNode 返回的 COMMITTED manifest，不能根据 ViewNode 观测或 StorageNode 本地 live chunk 推断对象可见。
- chunk 必须按 manifest 顺序和 offset 重建到临时输出文件，保持 bounded memory；每次只持有单个 bounded chunk payload。
- 每个 chunk 都必须校验 manifest checksum；任何 checksum mismatch、payload size 不一致、chunk 缺失或输出文件 IO 失败都必须 fail-fast。
- 所有 chunk 成功后必须再做一次对象级 checksum 校验；成功后才把临时文件 publish 到最终 `destination_path`。
- 失败时不得把部分文件声明为成功；临时输出文件应清理，避免把损坏下载结果伪装成已完成对象。

## Payload boundary

- 真实文件数据、chunk bytes、完整文件 buffer 只能经过 StorageNode data-plane。
- Raft log、Raft snapshot、metadata snapshot、MetadataNode command payload 和 object manifest 只能保存 metadata、checksum、size、version、chunk_id、node_id、offset、state 等事实。
- `WritePlan` 和 `CommitObject` 的输入输出不得包含真实 chunk payload。
- 任务报告、诊断输出和客户端摘要不得打印原始文件 payload。

## Bounded memory 要求

- upload / download 的内存上界必须由 chunk size、并发 chunk 数、RPC buffer 和文件 IO buffer 共同约束。
- 不允许为了计算 checksum、重试、CommitObject 或最终校验而把完整文件常驻内存。
- 对象级 checksum 应使用增量状态维护。
- 并发 chunk transfer 后续可扩展，但必须有明确的并发上限、背压或限流策略。
- 大文件路径必须和小文件路径保持同一 payload boundary，不允许为小文件开特殊的 Raft inline payload 路径。

## 与其他模块关系

### ViewNode

- ViewNode 提供 MetadataNode / StorageNode endpoint、leader hint 和健康观测。
- ViewNode 不是 object manifest authority，也不是 Raft membership authority。
- transfer 必须把 ViewNode 结果视为候选发现信息，并继续处理 MetadataNode 的权威响应。

### MetadataNode

- MetadataNode 是 object manifest、object state、version、commit visibility 和 Raft membership 的一致性权威。
- transfer 通过 MetadataNode 获取 `WritePlan`、提交 `CommitObject`、读取 COMMITTED manifest。
- transfer 不绕过 MetadataNode 判断对象是否可见。

### StorageNode

- StorageNode 保存真实 chunk payload，负责 checksum、durable publish、read、delete 和 restart recovery 的 data-plane contract。
- transfer 只能通过 StorageNode RPC / adapter 写入或读取 chunk。
- transfer 不直接访问 StorageNode 的本地 chunk 文件、索引、staging 或 publish 状态。

### storage_client

- `storage_client` 是用户入口，负责参数解析、配置加载、调用 transfer 编排并输出诊断。
- 复杂 upload / download 编排应在 transfer 模块内完成，避免 app 变成业务逻辑中心。

## T037 CLI 边界

- `storage_client upload/download` 只负责解析 CLI 参数、读取最小 client config、装配 ViewNode/transfer adapter 并输出结果。
- app 层可以基于 transfer result 决定退出码和成功/失败展示，但不能重新实现 discovery、Metadata RPC、StorageNode chunk IO 或对象可见性判断。
- upload 只有在 transfer 明确返回 `committed=true` 时才能被 CLI 声明为成功；不得把仅创建了 write plan 或仅完成部分 chunk 准备的结果误报为上传成功。
- download 只有在 transfer 明确返回最终 checksum verified 后才能输出 integrity `PASS`。

## 传输侧错误边界

- `NOT_LEADER`：应基于 leader hint 或发现结果重新选择 MetadataNode；重试策略必须保留上限和诊断。
- checksum mismatch：必须失败，不得提交成功或输出 PASS。
- IO failure：源文件读取、输出文件写入、StorageNode chunk read / write 都必须映射为明确失败。
- discovery failure：无法发现 MetadataNode 时 upload / download 失败；已有 COMMITTED manifest 的 chunk 读取是否需要 ViewNode，应由后续 adapter 策略明确。
- commit failure：chunk 已写入不代表对象可见；`CommitObject` 未成功时必须报告未提交状态，并可产生清理候选。
- partial transfer：必须能说明完成到哪个 chunk / byte，避免误报完整成功。

## 禁止事项

- 不保存 object manifest 的一致性权威副本。
- 不决定对象是否 COMMITTED 可见。
- 不修改 Raft membership，不参与 quorum、leader election 或 commit 规则。
- 不让真实 payload、chunk bytes、完整文件数据进入 Raft log、Raft snapshot 或 metadata snapshot。
- 不把 ViewNode 的观测信息当成对象可见性依据。
- 不直接修改 StorageNode 本地 chunk 状态。
- 不实现整文件常驻内存的上传或下载路径。

## 后续扩展点

- streaming RPC：可在独立协议变更任务中引入 client/server streaming chunk write / read，但必须继续禁止 payload 进入 Raft。
- retry policy：为 MetadataNode `NOT_LEADER`、StorageNode transient failure、超时和幂等 chunk write 定义有限重试和 backoff。
- 多副本读取：按 manifest 中 replica node_id 选择健康副本，支持 checksum mismatch 后切换副本并保留诊断。
- 并发 chunk transfer：增加有界并发、背压、限流和 per-session 资源预算。
- 限流和 QoS：按客户端、bucket、StorageNode 或全局带宽限制控制传输压力。
- resumable transfer：后续可在不改变 metadata authority 的前提下记录客户端侧断点状态。
- diagnostics：统一 request_id、node_id、leader hint、chunk index、checksum failure 和 partial progress 输出。

## 模块边界总结

- 本模块表达“客户端如何按权威 metadata 和 data-plane contract 传输真实文件”。
- MetadataNode 决定“对象 metadata 何时被一致性提交并可见”。
- StorageNode 决定“chunk payload 如何被真实保存、读取和恢复”。
- ViewNode 决定“节点如何被发现和观测”。

`modules/store/transfer/` 不能跨过这些边界变成新的 metadata authority、StorageNode 内部实现或 Raft payload 通道。
