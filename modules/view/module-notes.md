# ViewNode 模块说明

## 模块职责

`modules/view/` 负责 008 阶段的 ViewNode registry、gRPC service adapter、client adapter 和 discovery/observation 边界。

当前代码已经实现的能力主要有：

- 内存型 `ViewNodeRegistry`
- `RegisterNode` / `HeartbeatNode` / `DiscoverMetadata` / `DiscoverStorage` / `GetClusterView`
- liveness 推导、Metadata leader hint 观测、storage writable 过滤
- ViewNode gRPC service adapter
- ViewNode gRPC client adapter
- StorageNode 首次注册时基于 `data_dir_fingerprint` 的 `node_id` 分配/确认路径

它的定位始终是 discovery-only / observation-only，不是 Raft 或对象状态的权威节点。

## 当前关键结构和接口

### `view_registry.h`

当前 registry 暴露的核心类型和接口包括：

- 观测类型：
  - `NodeRegistration`
  - `ViewNodeSnapshot`
  - `MetadataLeaderHint`
  - `MetadataNodeObservation`
  - `ViewRegistryDiagnostic`
- 请求/结果类型：
  - `RegisterNodeRequest/Result`
  - `HeartbeatNodeRequest/Result`
  - `DiscoverMetadataRequest/Result`
  - `DiscoverStorageRequest/Result`
  - `GetClusterViewRequest/Result`
- 运行配置：
  - `ViewRegistryConfig`
  - 包括 `stale_timeout`、`suspect_timeout`、`dead_timeout`、endpoint 唯一性和 dead node 保留策略
- 对外接口：
  - `RegisterNode(...)`
  - `HeartbeatNode(...)`
  - `LookupNode(...)`
  - `DiscoverMetadata(...)`
  - `DiscoverStorage(...)`
  - `GetClusterView(...)`

### `view_service_impl.h`

`ViewNodeServiceImpl` 是 proto/view 与 `ViewNodeRegistry` 之间的 unary gRPC adapter：

- 只负责字段映射、时间注入和错误收敛
- 不承载 app 生命周期
- 不修改 membership、不决定对象可见性、不操作 payload

### `view_client.h`

`ViewNodeClient` 是调用方复用的 RPC client adapter：

- 统一 timeout / `wait_for_ready` 配置
- 返回 transport 诊断和 registry 语义结果
- 供 `metadata_node_app`、`storage_node_app`、`storage_client` 和 transfer 编排复用

## 当前实现流程

### registry

`view_registry.cpp` 当前是内存 registry，实现重点包括：

1. 注册请求校验：
   - `cluster_id`
   - `node_id`
   - `node_type`
   - endpoint
2. endpoint / `node_id` / `data_dir_fingerprint` 冲突诊断
3. heartbeat sequence 去旧
4. 根据 `last_seen_unix_ms` 和 registry timeout 推导：
   - `LIVE`
   - `STALE`
   - `SUSPECT`
   - `DEAD`
5. 生成 discovery snapshot 和 cluster view

Metadata 观测状态在快照阶段会做保守归一化：

- 已注册但 membership 未明确时，只展示为 `REGISTERED`
- `LEARNER` 只表示观测上的 learner，不等于 committed membership
- `DEAD` / `UNAVAILABLE` 会映射成 `DOWN`

这些都只是观测显示，不授予任何 Raft authority。

### service adapter

`view_service_impl.cpp` 负责：

- proto 枚举和本地枚举之间的转换
- gRPC request/response 与 registry 类型之间的映射
- `now_unix_ms` 注入
- 异常收敛成明确的 gRPC internal failure

其中 StorageNode first registration 的 `node_id` 路径已经接入：

- 当 StorageNode 注册请求 `node_id` 为空时，service 会要求非空 `data_dir_fingerprint`
- 使用 `cluster_id + '\n' + data_dir_fingerprint` 做 FNV-1a 64 计算
- 生成稳定 `store-<hex>` 形式的 `node_id`
- 如果该 fingerprint 已绑定既有 storage registration，则走 confirm-existing
- 如果同 fingerprint 或已分配 `node_id` 指向不兼容记录，则返回冲突

这条路径只解决“ViewNode 侧 StorageNode 首次注册 identity allocation/confirmation”，不意味着：

- 修改 Raft membership
- 给 MetadataNode 分配 `raft_id`
- 改写本地 `node.identity`

### client adapter

`view_client.cpp` 当前负责：

- 统一构造 unary RPC context
- 设置 deadline / `wait_for_ready`
- 解析 proto summary / warning / snapshot / leader hint
- 把 proto warning code 映射到本地 `ViewRegistryIssueCode`
- 输出 transport 诊断：
  - gRPC status code
  - error message/details
  - effective timeout
  - retryable

client adapter 只做 transport + proto 映射，不做强一致推理。

## 关键边界

### discovery / observation only

ViewNode 可以：

- 返回 MetadataNode 候选 endpoint
- 返回 leader hint
- 返回 StorageNode endpoint / 容量 / health / load 快照
- 返回 cluster view

ViewNode 不可以：

- 保存 object manifest 权威副本
- 决定对象是否 `COMMITTED` 可见
- 参与 `CommitObject`
- 修改 Raft membership
- 缩小 quorum
- 参与 leader election
- 直接读写 StorageNode chunk payload

### MetadataNode registration 与 voter membership 的边界

MetadataNode 在 ViewNode 中可以带：

- `raft_id`
- `raft_role`
- `membership_state`
- `leader_hint`
- `term`
- `commit_index`
- `membership_epoch`

但这些都只是“被上报或被观测到的事实”。即使 `membership_state` 显示为 `VOTER`，其权威来源也只能是 Metadata/Raft 自己的已提交状态，不是 ViewNode。

### StorageNode first registration / node_id allocation 边界

这条路径只允许用于 StorageNode：

- 依赖 `data_dir_fingerprint` 做稳定 node_id 分配或确认
- 解决首次注册 node_id 缺失的问题

误用风险主要有两类：

- 把它当成 MetadataNode / ViewNode 的通用 identity authority
- 把“已在 ViewNode 注册成功”误解释成“已持久化本地 identity”

当前实现都没有这么做，后续维护也不能越界。

## 与其他模块的交互

- `apps/metadata_node_app.cpp`
  - 启动后用 `ViewNodeClient` 注册自己并上报 leader/quorum 观测信息
- `apps/storage_node_app.cpp`
  - 启动后用 `ViewNodeClient` 注册自己并做 heartbeat
- `modules/store/placement`
  - 只消费 ViewNode 暴露的 StorageNode snapshot 作为 placement 候选输入
- `modules/store/transfer`
  - 只把 `DiscoverMetadata` / `DiscoverStorage` 结果当成 endpoint 候选，不当成对象可见性 authority

## 容易误用的点

- 把 leader hint 当成强一致 leader 事实
  - 当前实现明确要求调用方继续处理 `NOT_LEADER`
- 把 registry 的 membership 观测状态当成 committed voter 集合
  - 这会直接破坏 quorum 边界
- 把 `DiscoverStorage` 的 live/healthy 结果当成对象可见性依据
  - transfer 和 client 都不能这么做
- 把 StorageNode first registration allocation 当成通用 node identity 服务
  - 当前只支持 StorageNode 的 first registration path

## 当前状态和后续边界

- 已实现：
  - registry
  - service/client adapter
  - liveness 计算
  - leader hint 观测
  - StorageNode first registration / confirmation path
- 未实现：
  - ViewNode 自身高可用或复制
  - registry 持久化
  - 多 ViewNode 共识
  - 认证授权和租户隔离

后续扩展这些能力时，仍必须保持“ViewNode 只负责 discovery/observation，不负责 metadata authority、membership authority 和 payload path”这条硬边界。
