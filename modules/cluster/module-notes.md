# cluster 模块说明

## 模块职责

`modules/cluster/` 负责 008 阶段统一的 cluster / config / identity 公共基础边界。

当前规划中，本模块负责：

- `ClusterConfig` 的结构表达、配置生成、配置加载、配置校验边界
- 统一描述 ViewNode、MetadataNode、StorageNode 的数量、endpoint、`data_dir`、capacity、timeout、chunk policy
- 初始 Raft membership 的配置生成边界，支持 1/3/5/7 个 MetadataNode voter 配置
- `NodeIdentity` / `node.identity` 的持久身份边界，包括首次创建、重启复用、identity/config mismatch 检查
- Linux / Windows 路径、flush、atomic publish、directory durability 等跨平台差异说明

本模块不直接承载运行中的业务编排；它提供的是拓扑、身份和 durability contract 的公共基础。

## 核心概念

### `ClusterConfig`

- 描述一次集群启动的目标拓扑和关键运行参数。
- 应统一表达：
  - `cluster_id`
  - ViewNode / MetadataNode / StorageNode 列表
  - endpoint
  - `data_dir`
  - `snapshot_dir`
  - capacity
  - timeout
  - chunk policy
  - 初始 Raft membership
- 这里负责“配置正确性”和“配置可生成”，不负责运行时共识推进。

### `NodeIdentity`

- 描述节点本地持久身份。
- 应至少能表达：
  - `node_id`
  - `node_type`
  - 可选 `raft_id`
  - `cluster_id`
  - identity version
  - 创建来源和时间
- 关注点是：
  - 首次创建
  - 重启复用
  - 与配置不一致时显式报错
  - durable write contract
- 当前 `node_identity.h` 只定义类型和接口边界：
  - `NodeIdentity` 表达持久身份。
  - `ExpectedNodeIdentity` 表达启动配置对本地身份的匹配期望。
  - `NodeIdentityLoadOptions` / `NodeIdentityStoreOptions` 表达 load / store 的输入边界。
  - `NodeIdentityLoadResult` / `NodeIdentityStoreResult` / `NodeIdentityLoadOrCreateResult` 表达结果、诊断和 durability 状态边界。
  - `ValidateNodeIdentity` / `ValidateNodeIdentityMatches` / `LoadNodeIdentity` / `StoreNodeIdentity` / `LoadOrCreateNodeIdentity` 仅声明接口，不在头文件实现文件 IO。
- `node.identity` 当前采用稳定的文本 `key=value` 格式，至少包含 `identity_version`、`cluster_id`、`node_id`、`node_type`、`raft_id`、`created_at_unix_ms`、`source` 字段；格式损坏、缺字段、重复字段或与期望配置冲突时必须返回明确诊断，不能静默覆盖。
- MetadataNode 必须区分 `node_id` 与 `raft_id`；StorageNode / ViewNode 不应携带 `raft_id`。
- `node.identity` 已存在且与期望冲突时必须显式失败，不得静默覆盖。

### `RaftMembership`

- 在本模块中只作为“初始配置生成边界”和“静态校验对象”出现。
- 本模块可以生成或校验初始 voter / learner 配置。
- 本模块不是运行时 membership authority。
- 运行中 membership change 只能由 Raft 自己通过已提交日志生效。

### endpoint

- endpoint 用于统一描述节点监听地址和服务发现入口。
- 本模块负责 endpoint 唯一性、格式合理性、角色归属的一致性校验。
- 本模块不负责 endpoint 健康状态观测，也不负责发现结果分发。
- 当前 `ClusterConfig` 只有单一 `endpoint` 字段，因此配置生成阶段不能静默分裂 `bind_host` / `advertise_host`。
- 在未引入独立 listen/advertise 字段前，生成器应要求 `advertise_host` 为空或与 `bind_host` 一致，并对不一致输入返回明确错误。

### `data_dir`

- `data_dir` 是本地 durable state 的根路径边界。
- 本模块负责：
  - 路径合法性校验
  - 不同节点目录冲突检查
  - identity 所属目录和角色匹配关系
- 本模块不负责具体 chunk、Raft log、snapshot 的读写实现。

### capacity

- capacity 用于表达 StorageNode 的静态或初始配置能力。
- 本模块只负责配置表达与基础校验。
- 真正的运行时容量变化、健康状态和负载属于 ViewNode / StorageNode 观测事实，不由本模块决定。

### durability contract

- 本模块负责说明 identity 和配置相关持久化文件的 durability contract。
- 必须明确：
  - 临时文件写入
  - flush
  - atomic publish
  - directory durability
  - 平台差异
- 不允许 required durability operation 出现 no-op success。

## 禁止事项

- 不实现 Raft 共识、leader election、AppendEntries、InstallSnapshot。
- 不直接修改已提交的 Raft membership。
- 不决定对象是否 COMMITTED 可见。
- 不保存 object manifest 的一致性权威副本。
- 不处理真实 chunk payload 或文件内容。
- 不替代 ViewNode 的服务发现、节点注册、心跳观测。
- 不替代 StorageNode 的 chunk 落盘、publish、checksum、restart recovery 逻辑。
- 不把配置生成器写成混合业务入口，不把 MetadataNode / ViewNode / StorageNode 的业务逻辑下沉到本模块。

## Linux / Windows 路径与 durability 注意点

- 路径处理应优先使用跨平台语义，不把 Linux 专有路径假设写死进共享配置逻辑。
- Linux 上，如果 contract 声明要求 durable publish，应明确 file flush 和 directory durability 的要求。
- Linux 当前实现边界是：临时文件写入 -> 文件 `fsync` -> 原子 publish -> `data_dir` 目录 `fsync`，只有全部完成后才可报告 durable success。
- Windows 当前实现边界是：临时文件写入 -> `FlushFileBuffers` -> `MoveFileExW(MOVEFILE_WRITE_THROUGH)` 发布；由于独立目录 durability 还没有等价实现，`required` 模式必须返回明确 `durability_error`，只允许 `best_effort_for_tests` 以 `durable=false` 成功返回，禁止静默伪装成 durable success。
- identity 文件写入必须有“写入中”和“已发布”边界，避免崩溃后把半写状态当成有效身份。
- `data_dir`、identity 文件名和路径拼接规则必须保持可诊断，避免平台差异导致身份漂移。

## 后续扩展点

- 运行时动态 membership 接口边界：
  - 本模块只保留配置和参数表达边界，不直接实现运行时 AddRaftNode / RemoveRaftNode / PromoteLearner。
- 配置热加载：
  - 可预留接口边界，但热加载不应绕过现有 identity / durability / ownership 校验。
- 多 ViewNode 配置：
  - 本模块可以描述多个 ViewNode endpoint 和角色配置，但不赋予其一致性权威。
- 配置生成器：
  - 后续可扩展为生成本地开发、测试、基准和多节点部署配置，但必须保持结果可重复、可校验、可追踪。

## 模块边界总结

- 本模块表达“系统应该如何被配置和识别”。
- Raft 模块决定“哪些 metadata 被一致性提交”。
- ViewNode 模块决定“节点如何被发现和观测”。
- StorageNode 模块决定“chunk 如何被真实保存和恢复”。

`modules/cluster/` 不能跨过这些边界变成新的业务中心。
