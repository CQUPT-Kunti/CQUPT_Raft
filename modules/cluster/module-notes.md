# cluster 模块说明

## 模块职责

`modules/cluster/` 负责 008 阶段统一的 cluster config、单节点解析、初始 Raft quorum 诊断和本地 `node.identity` 持久身份边界。

当前代码已经实现的职责主要有：

- `ClusterConfig` / `ClusterConfigGenerationRequest` / `ResolvedClusterNodeConfig` 等配置类型
- 确定性配置生成、endpoint 分配、文本配置序列化与加载
- 初始 MetadataNode voter / learner membership 校验与 quorum helper
- `node.identity` 的 load / store / load-or-create
- identity/config mismatch、raft_id mismatch、durability publish failure 等诊断

本模块只负责“配置和本地身份”这层基础能力，不负责运行时业务 authority。

## 当前关键结构和接口

### `cluster_config.h`

当前对外边界主要包括：

- `ValidateClusterConfig(...)`
  - 校验 `cluster_id`、节点数量、`node_id`、`endpoint`、`data_dir`、`snapshot_dir`、capacity、chunk policy、timeout policy、raft_id 和初始 membership 一致性
- `AllocateClusterEndpoints(...)`
  - 仅根据 generation request 生成稳定 endpoint 分配结果
  - 不启动节点，不做 discovery
- `GenerateDeterministicClusterConfig(...)`
  - 生成可重复、可诊断的完整 `ClusterConfig`
  - MetadataNode 的 `node_id` 与 `raft_id` 在这里锁定，后续 app 只能校验，不能运行时重写
- `ResolveClusterNodeConfig(...)`
  - 按 `node_type + node_id` 精确解析单节点配置
  - 不允许 fallback 到“第一个节点”或硬编码 demo 拓扑
- `ComputeInitialRaftQuorum(...)` / `ComputeInitialRaftQuorumSize(...)`
  - 只根据 initial voter membership 计算 election / commit quorum
  - 这是配置校验和诊断 helper，不是运行时 membership authority
- `LoadClusterConfigFromJsonFile(...)` / `SerializeClusterConfigToJson(...)`
  - 负责文本配置文件读写边界
  - 不承载 app 生命周期和业务逻辑

### `node_identity.h`

当前对外边界主要包括：

- `NodeIdentity`
  - 持久身份最少包含：`cluster_id`、`node_id`、`node_type`、可选 `raft_id`、`identity_version`、`created_at_unix_ms`、`source`
- `ExpectedNodeIdentity`
  - 表达 app 启动时对本地身份的匹配期望
  - 非空期望不匹配时必须显式失败，不能静默覆盖
- `LoadNodeIdentity(...)`
  - 只负责读取和校验 `node.identity`
- `StoreNodeIdentity(...)`
  - 只负责持久化写入与 durability publish
- `LoadOrCreateNodeIdentity(...)`
  - 先尝试读取既有 identity；不存在时再创建
  - 已有 identity 与期望不一致时失败，不会偷偷重建
- `ValidateNodeIdentity(...)` / `ValidateNodeIdentityMatches(...)`
  - 做纯校验，不做文件 IO

## 当前实现流程

### 配置生成与解析

1. generation request 提供节点数量、固定 `node_id` / `raft_id`、port base、`base_dir`、chunk policy、timeout 等输入
2. `AllocateClusterEndpoints(...)` 先生成稳定 endpoint 分配
3. `GenerateDeterministicClusterConfig(...)` 组合：
   - ViewNode 配置
   - MetadataNode 配置
   - StorageNode 配置
   - initial Raft membership
4. `ValidateClusterConfig(...)` 统一校验
5. app 启动时通过 `LoadClusterConfigFromJsonFile(...)` + `ResolveClusterNodeConfig(...)` 恢复自己的单节点配置

### `node.identity` 读写与复用

1. `ResolveNodeIdentityPath(data_dir)` 固定 identity 文件路径
2. `LoadNodeIdentity(...)` 解析文本 `key=value` 文件并做结构校验
3. `ValidateNodeIdentityMatches(...)` 校验本地 identity 与启动配置期望是否一致
4. `StoreNodeIdentity(...)` 通过临时文件写入 + publish 完成身份持久化
5. `LoadOrCreateNodeIdentity(...)` 在“读已有”和“首次创建”之间做明确分支

当前实现明确区分：

- MetadataNode：
  - 必须区分 `node_id` 与 `raft_id`
  - 缺 `raft_id`、重复 `raft_id` 或与 config 不匹配都会失败
- StorageNode / ViewNode：
  - 不应携带 `raft_id`
  - 若本地 identity 带了 `raft_id`，会按 mismatch/validation failure 处理

## 跨平台 durability 边界

当前 `node.identity` 的 durable publish contract 已经落在实现里：

- Linux：
  - 临时文件写入
  - 文件 flush / `fsync`
  - 原子 publish
  - `data_dir` 目录 `fsync`
  - 只有全部完成后才能报告 durable success
- Windows：
  - 临时文件写入
  - `FlushFileBuffers`
  - `MoveFileExW(MOVEFILE_WRITE_THROUGH)` 发布
  - 由于独立目录 durability 还没有完全等价实现，`kRequired` 模式必须返回明确 `durability_error`
  - 只允许 `kBestEffortForTests` 以 `durable=false` 返回受限成功

这里的核心约束是：required durability operation 不能 silent no-op success。

## 非职责边界

本模块不负责：

- Raft leader election、log replication、commit、snapshot、runtime membership change
- ViewNode 注册、心跳、leader hint、cluster view
- StorageNode chunk 写入、publish、checksum、restart recovery
- 对象 `PENDING/COMMITTED/DELETED` 可见性
- 真实 payload、chunk bytes 或整文件内容

尤其要注意：

- `ComputeInitialRaftQuorum(...)` 不是运行时 quorum authority
- `ClusterConfig` 不是动态 membership change API
- `node.identity` 不会因为 ViewNode 注册结果或 CLI override 被静默改写

## 容易误用的点

- 把 `node_id` 和 `raft_id` 混为一谈：
  - MetadataNode 必须同时维护二者的语义边界
- 把 config generator 生成的 membership 当成运行时可修改状态：
  - 当前模块只生成和校验初始配置
- 用“默认节点”替代显式 `ResolveClusterNodeConfig(...)`：
  - 当前实现明确禁止这种 fallback
- 把 Windows best-effort 路径误写成 durable success：
  - 这会直接破坏 identity/restart contract

## 与其他模块的交互

- `apps/metadata_node_app.cpp`
  - 使用 cluster 模块解析 config，并校验 config-generated `node_id` / `raft_id`
- `apps/storage_node_app.cpp` / `apps/view_node_app.cpp`
  - 使用 `LoadOrCreateNodeIdentity(...)` 加载或创建稳定身份
- `modules/view/`
  - 只能消费这里生成/校验过的稳定身份事实，不能反向改写本地 identity
- `modules/raft/node`
  - 只读取 config-generated Raft 身份和初始 membership，不由 cluster 模块驱动运行时变更

## 当前状态和后续边界

- 已实现：
  - 确定性配置生成
  - 单节点配置解析
  - 初始 quorum helper
  - durable identity load/store/load-or-create
  - identity conflict diagnostics
- 未实现：
  - 运行时动态 membership change
  - config 热加载
  - 跨节点分布式 identity 协调

后续如果扩展这些能力，仍必须保持“cluster 只负责配置和本地身份，不负责运行时 authority”这条硬边界。
