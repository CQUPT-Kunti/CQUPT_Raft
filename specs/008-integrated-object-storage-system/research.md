# Research: Integrated Object Storage System

**Feature**: 008-integrated-object-storage-system  
**Date**: 2026-06-05

## Decision 1: ViewNode 是发现与观测组件

**Decision**: ViewNode 负责节点注册、服务发现、地址/端口、容量、健康状态、心跳、存活状态、Raft 节点观测状态和 leader hint 展示。ViewNode 不保存对象 manifest，不参与对象 commit，不直接操作 StorageNode，不直接修改 Raft membership。

**Rationale**: 本阶段需要解决客户端发现与集群观测问题，而对象可见性和 Raft membership 必须由 Raft 自己的已提交状态保证。把 ViewNode 做成一致性权威会扩大范围并引入新的 split-brain 风险。

**Alternatives considered**:

- ViewNode 也保存对象 manifest：拒绝，会复制 metadata authority 并绕过 Raft。
- ViewNode 直接管理 Raft membership：拒绝，会破坏 Raft membership change 的安全边界。
- 暂不引入 ViewNode：拒绝，无法满足配置驱动服务发现和动态 StorageNode 注册。

## Decision 2: Raft quorum 基于已提交 membership voter 总数

**Decision**: commit quorum 和 election quorum 按已提交 Raft membership 中 voter 总数计算。1/3/5/7 voter 的 quorum 分别为 1/2/3/4。当前存活节点减少不会降低 quorum。

**Rationale**: 这是避免 split-brain 的核心安全要求。若 3 个 voter 只剩 1 个还允许提交，会破坏强一致对象 manifest 与 commit 可见性。

**Alternatives considered**:

- 按当前 live 节点数计算 quorum：拒绝，会在网络分区下产生多个合法提交方。
- ViewNode 根据健康状态调整 quorum：拒绝，ViewNode 不是 Raft authority。
- 第一阶段只支持 3 节点：拒绝，用户明确要求 1/3/5/7 配置驱动。

## Decision 3: 第一阶段 Raft membership 静态生成，动态变更保留边界

**Decision**: 第一阶段由配置生成器或 cluster config 生成 MetadataNode 初始 voter membership。新 Raft 节点可以向 ViewNode 注册为 REGISTERED/JOINING/LEARNER/VOTER/DOWN 等观测状态，但 AddRaftNode、RemoveRaftNode、PromoteLearner 不作为第一阶段强制实现。

**Rationale**: 运行时 membership change 涉及 learner 日志追赶、snapshot 安装、joint consensus 或等价安全流程，范围大且高风险。先明确边界可避免把 ViewNode 误实现成 membership 管理中心。

**Alternatives considered**:

- 第一阶段实现完整动态 voter 添加：暂缓，风险与范围超过端到端对象存储 MVP。
- 由 ViewNode 注册即加入 Raft：拒绝，违反 Raft 安全。
- 手工修改代码添加 Raft 节点：拒绝，不满足配置驱动。

## Decision 4: StorageNode 支持动态注册和 placement 资格更新

**Decision**: StorageNode 启动后向 ViewNode 注册 node_id、endpoint、capacity、health、load、failure domain 和 heartbeat。MetadataNode 生成 WritePlan 时只选择健康、live、容量满足、未过载且符合副本策略的 StorageNode。

**Rationale**: StorageNode 扩展是数据面横向扩容的第一步，且已有 `modules/store/node/StorageNodeRegistry` 与 `modules/store/placement/PlacementManager` 可作为事实和策略基础。

**Alternatives considered**:

- StorageNode 列表写死在 MetadataNode 配置中：拒绝，扩容需要改配置甚至改代码。
- Client 自己选择 StorageNode：拒绝，placement 应由 metadata control-plane 统一生成，Client 只执行计划。
- ViewNode 直接下发写入：拒绝，ViewNode 不操作数据面。

## Decision 5: node_id 自动分配并持久化

**Decision**: StorageNode 首次启动没有 `node.identity` 时，通过 ViewNode 或配置生成流程获得 node_id，并在 data_dir 下 durable write identity 后才接受 placement。后续重启必须复用 identity。Raft MetadataNode 的 node_id/raft_id 第一阶段由配置生成器生成。

**Rationale**: metadata manifest 记录 chunk 所在 node_id。如果重启生成新身份，已提交 manifest 将指向失效副本。Raft node identity 更敏感，必须由受控配置生成。

**Alternatives considered**:

- 每次启动随机生成 node_id：拒绝，会破坏 manifest。
- 要求用户手工维护所有 node_id：不优先，容易出错；仅作为显式 override 或配置生成结果。
- ViewNode 为 Raft 节点即时分配 voter id：拒绝，不能绕过 membership authority。

## Decision 6: 上传流程使用 WritePlan -> chunk write -> CommitObject

**Decision**: Client 先从 ViewNode 发现 MetadataNode，向 leader 或可重定向 MetadataNode 申请写入计划；MetadataNode 生成 PENDING 对象、chunk layout 和 placement；Client 按计划写 StorageNode；StorageNode 返回 chunk_id、node_id、size、checksum、durable；Client 提交 CommitObject；Raft commit 成功后对象可见。

**Rationale**: 这保留了真实 payload 在数据面，同时让 object manifest 和可见性由 Raft 统一控制。

**Alternatives considered**:

- Client 先写 StorageNode 再让 MetadataNode 反查：拒绝，orphan 数据更多且 placement authority 不清晰。
- MetadataNode 代理所有 chunk payload：拒绝，会把大流量引入 control-plane。
- StorageNode 写成功即对象可见：拒绝，绕过 Raft commit。

## Decision 7: 下载流程使用 manifest 和端到端 checksum

**Decision**: Client 通过 ViewNode 找 MetadataNode，读取 COMMITTED object manifest，然后按 ChunkManifest 访问 StorageNode，逐 chunk 校验 checksum，拼接输出文件，最后比对对象级 SHA-256 或 etag。

**Rationale**: manifest 是强一致元数据，StorageNode 是数据源。双层校验能同时发现单 chunk 损坏和整体拼接错误。

**Alternatives considered**:

- 只信任 StorageNode 返回数据：拒绝，无法验证数据是否符合 manifest。
- 只校验最终文件：不充分，难以定位坏 chunk。
- ViewNode 返回 manifest：拒绝，ViewNode 不保存 object manifest。

## Decision 8: 真实 payload 边界

**Decision**: Raft 只保存 metadata、WritePlan、object manifest、chunk manifest、checksum、size、version、commit state。真实文件数据和 chunk payload 只进入 StorageNode write/read path。

**Rationale**: Raft log 和 snapshot 必须保持 metadata-only，否则会破坏性能、恢复时间和持久化边界。

**Alternatives considered**:

- 小文件内联进 Raft：拒绝，会形成例外路径并污染 snapshot。
- 在 metadata snapshot 中保存 chunk 内容摘要以外数据：拒绝，payload 不属于 metadata。

## Decision 9: 大文件使用 bounded chunk 流程，流式 RPC 作为后续优化

**Decision**: 第一阶段要求 Client 以固定 chunk 大小读取文件并逐 chunk 写入 StorageNode，不允许整文件进内存。现有 unary chunk RPC 可承载单个 bounded chunk；如果后续需要真正 client/server streaming RPC，必须作为独立协议变更任务处理。

**Rationale**: 用户目标是不要一次性把整个文件塞入内存。当前契约已有 chunk 粒度 write/read，可以先满足 bounded memory。直接引入 streaming proto 会扩大协议变更面。

**Alternatives considered**:

- 立即把 WriteChunk 改为 streaming：暂缓，需要跨 client/server/test 全面协议变更。
- 继续在 upload coordinator 中拼接 object_payload 算 etag：拒绝，需要改为流式对象 checksum 或要求调用方提供对象 checksum。

## Decision 10: 跨平台 durability contract 显式化

**Decision**: node.identity、StorageNode chunk publish、Raft metadata durability 相关 required operations 必须在 Linux 使用真实 fsync/rename/dir sync 语义，在 Windows 使用 FlushFileBuffers/等价发布语义或返回明确错误/记录较弱 contract；不允许 no-op success。

**Rationale**: 用户明确要求 Linux/Windows 跨平台，且项目 constitution 禁止静默降级。

**Alternatives considered**:

- 只在 Linux 实现 durability，Windows no-op：拒绝。
- 文档不区分平台：拒绝，无法验收。

## Decision 11: 独立 app 保持薄入口

**Decision**: 规划 `view_node_app`、`metadata_node_app`、`storage_node_app`、`storage_client`，可选 `storage_bench`。app 只负责参数、配置加载、依赖装配、启动和退出码，不承载业务分叉。

**Rationale**: 独立进程是端到端系统验收的必要条件，但业务逻辑应留在模块内，便于测试和维护。

**Alternatives considered**:

- 扩展 `raft_demo`：拒绝，会继续固定 demo 拓扑。
- 让 client 直接链接内部状态机完成上传：拒绝，绕过 RPC/进程边界。

## Decision 12: 测试以端到端真实文件为验收核心

**Decision**: 新增 E2E、quorum、recovery、concurrency、ViewNode discovery、identity 测试，并通过 CTest label 组织。测试日志仍遵守仓库输出规则，失败摘要写入本地日志或 task-reports。

**Rationale**: 这一阶段的风险在跨组件边界，单测不足以证明用户价值。

**Alternatives considered**:

- 只测 upload coordinator 单元：不充分，无法覆盖进程、发现、真实 chunk 和 manifest。
- 只做手动 quickstart：不充分，不能防回归。
