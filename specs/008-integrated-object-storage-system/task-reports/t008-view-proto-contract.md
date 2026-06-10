# T008 任务报告：ViewNode protobuf contract

## 1. 修改了哪些文件

- `proto/view.proto`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t008-view-proto-contract.md`

## 2. proto/view.proto 新增了哪些契约能力

本次新增了独立的 `proto/view.proto`，`package` 采用 `view`，没有修改现有 `raft.proto`、`metadata.proto`、`storage_node.proto` 或 `common.proto`。

新增契约能力包括：

- `service ViewNodeService`
  - `RegisterNode`
  - `HeartbeatNode`
  - `DiscoverMetadata`
  - `DiscoverStorage`
  - `GetClusterView`

- ViewNode 专用状态与节点模型
  - `ViewNodeStatusCode`
  - `ViewNodeType`
  - `ViewNodeLivenessState`
  - `ViewNodeHealth`
  - `ViewNodeDiskPressure`
  - `MetadataMembershipObservedState`
  - `MetadataRaftObservedRole`

- ViewNode 专用公共消息
  - `ViewNodeResponseSummary`
  - `ViewNodeFailureDomain`
  - `ViewNodeCapacityReport`
  - `ViewNodeHealthReport`
  - `ViewNodeLoadReport`
  - `MetadataLeaderHint`
  - `MetadataNodeObservation`
  - `ViewNodeRegistration`
  - `ViewNodeSnapshot`
  - `ClusterViewWarning`

这些结构一起表达了节点注册、心跳、MetadataNode discovery、StorageNode discovery 和全量集群观测视图所需的外部协议边界。

## 3. 每个核心 RPC 的作用和边界

### RegisterNode

请求：

- `RegisterNodeRequest`
  - `request_id`
  - `registration`

核心注册载荷：

- `ViewNodeRegistration`
  - `cluster_id`
  - `node_id`
  - `node_type`
  - `endpoint`
  - `control_plane_endpoint`
  - `data_plane_endpoint`
  - `data_dir_fingerprint`
  - `observed_at_unix_ms`
  - `failure_domain`
  - `health`
  - `capacity`
  - `load`
  - `metadata`

响应：

- `RegisterNodeResponse`
  - `summary`
  - `created`
  - `idempotent`
  - `conflict`
  - `snapshot`
  - `warnings`

作用：

- 表达 ViewNode / MetadataNode / StorageNode 的首次注册或重复注册。
- 支持注册成功、幂等重放、冲突注册和诊断 warning。

边界：

- `metadata` 字段只表示 MetadataNode 的观测事实。
- 注册成功不等于加入 Raft membership。
- 不能把 RegisterNode 当作 AddRaftNode、PromoteLearner 或 voter 授权接口。

### HeartbeatNode

请求：

- `HeartbeatNodeRequest`
  - `request_id`
  - `cluster_id`
  - `node_id`
  - `node_type`
  - `sequence`
  - `observation`

响应：

- `HeartbeatNodeResponse`
  - `summary`
  - `accepted_sequence`
  - `applied`
  - `idempotent`
  - `stale_ignored`
  - `snapshot`

作用：

- 表达周期性 heartbeat 上报。
- 用 `sequence` 支持过期 heartbeat 忽略和有效 heartbeat 接受。
- 刷新 health、capacity、load、leader hint 和 liveness 相关观测事实。

边界：

- Heartbeat 只更新 ViewNode 观测状态。
- Heartbeat 不改变 quorum，不改变 leader election 规则，也不改变对象可见性。

### DiscoverMetadata

请求：

- `DiscoverMetadataRequest`
  - `request_id`
  - `cluster_id`
  - `prefer_leader`
  - `live_only`
  - `limit`

响应：

- `DiscoverMetadataResponse`
  - `summary`
  - `metadata_nodes`
  - `leader_hint`
  - `observed_at_unix_ms`
  - `membership_epoch`
  - `warnings`

作用：

- 让 Client 或管理工具发现可用 MetadataNode。
- 支持 “偏好 leader” 的发现语义。
- 返回 leader hint 和 membership_epoch 作为观测诊断信息。

边界：

- `metadata_nodes` 和 `leader_hint` 只是候选地址和观测事实。
- Client 后续仍必须处理 MetadataService 的 `NOT_LEADER`。
- `membership_epoch` 是观测信息，不是 ViewNode 对 membership 的权威裁定。

### DiscoverStorage

请求：

- `DiscoverStorageRequest`
  - `request_id`
  - `cluster_id`
  - `live_only`
  - `minimum_available_capacity_bytes`
  - `zone`
  - `rack`
  - `limit`
  - `require_writable`

响应：

- `DiscoverStorageResponse`
  - `summary`
  - `storage_nodes`
  - `observed_at_unix_ms`
  - `warnings`

作用：

- 让 Client 或后续 placement 逻辑拿到 StorageNode 的观测快照。
- 支持最基本的 live/capacity/failure-domain 过滤边界。

边界：

- 返回的是 StorageNode facts，不是对象 manifest。
- 不能把 DiscoverStorage 当成对象是否可见、chunk 是否已提交的权威来源。
- 对象可见性只能来自 MetadataNode 的 `COMMITTED` manifest。

### GetClusterView

请求：

- `GetClusterViewRequest`
  - `request_id`
  - `cluster_id`
  - `include_dead_nodes`
  - `include_warnings`

响应：

- `GetClusterViewResponse`
  - `summary`
  - `view_nodes`
  - `metadata_nodes`
  - `storage_nodes`
  - `leader_hint`
  - `observed_at_unix_ms`
  - `warnings`

作用：

- 暴露完整集群观测视图，供后续 `status`、诊断和测试使用。

边界：

- 这是观测接口，不是 manifest authority，也不是 membership authority。
- 返回 warning 用于 identity 冲突、过期记录等诊断，不改变 Raft 或对象状态。

## 4. 如何体现 ViewNode discovery-only / observation-only / non-authority 边界

本次主要通过以下方式体现边界：

- 文件顶层注释直接声明：
  - ViewNode 只负责 discovery / observation
  - 注册不等于加入 Raft membership
  - 不改变 quorum、commit、leader election
  - 不保存 object manifest 权威副本
  - 不承载 payload / chunk bytes / 完整文件内容

- `MetadataMembershipObservedState` 的命名明确写成 `ObservedState`
  - 强调它是 ViewNode 看到的状态，不是权威 membership

- `MetadataNodeObservation` 和 `ViewNodeRegistration` 上方注释明确声明：
  - `metadata` 字段只表达观测事实
  - 不能据此直接把节点提升为 voter
  - 不能修改已提交 membership

- `MetadataLeaderHint` 注释明确：
  - leader hint 只是观测信息
  - Client 仍必须处理 `NOT_LEADER`

- `DiscoverStorageResponse` 注释明确：
  - 返回的是 StorageNode facts
  - 对象可见性只能来自 MetadataNode 的 `COMMITTED` manifest

- `GetClusterViewResponse` 注释明确：
  - 仅用于 status、诊断和测试观测
  - 不是 manifest 或 membership 的权威来源

## 5. 是否保持 existing proto 语义不变

- 是。
- 未修改：
  - `proto/raft.proto`
  - `proto/metadata.proto`
  - `proto/storage_node.proto`
  - `proto/common.proto`
- 没有改动任何 existing field number、message、enum 或 service 语义。
- `proto/view.proto` 是一个纯新增的 additive contract。
- 本次没有接入 CMake、没有生成 `view_proto` target；这属于 T009 范围，不作为 T008 失败条件。

## 6. 是否发现不合理点 / 警告 / 风险

- 现有 `common.proto` 中的 `MetadataLeaderHint` 和 `MetadataResponseSummary` 更偏 metadata RPC 返回语义，不完全适合直接复用到 ViewNode discovery contract；本次因此选择在 `view.proto` 内定义独立的 `MetadataLeaderHint` 和 `ViewNodeResponseSummary`，以保持 target 和协议边界清晰。
- `storage_node.proto` 已经有一套偏 StorageNode RPC 的 health/capacity/load 类型；本次没有跨 proto import 复用，避免在 T008 就把未来 `view_proto` 设计成必须绑定 `storage_node_proto` 的生成依赖。
- `ViewNodeRegistration` 同时保留了 `endpoint`、`control_plane_endpoint`、`data_plane_endpoint`，是为了兼容后续 MetadataNode / StorageNode / ViewNode 不同角色的 endpoint 表达；真正如何填充和校验留给后续实现任务。

## 7. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次仅新增 additive proto contract，没有改变已有行为，也没有引入新的风险类别。

## 8. 验证命令和结果

### 验证命令

```bash
git diff -- proto/view.proto specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t008-view-proto-contract.md
git status --short -- proto/view.proto specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t008-view-proto-contract.md
protoc --proto_path=proto --descriptor_set_out=/tmp/cqupt-raft-proto-check/view.pb proto/view.proto
rg -n "service .*Service|package .*;|enum .*StatusCode|ResponseSummary" proto/*.proto
```

### 验证结果

- `git diff -- ...` 按预期展示了 `tasks.md` 中 T008 从 `[ ]` 更新为 `[X]` 的修改。
- `git status --short -- ...` 确认：
  - `M specs/008-integrated-object-storage-system/tasks.md`
  - `?? proto/view.proto`
  - `?? specs/008-integrated-object-storage-system/task-reports/t008-view-proto-contract.md`
- `protoc --proto_path=proto --descriptor_set_out=/tmp/cqupt-raft-proto-check/view.pb proto/view.proto` 实际执行成功，说明 `proto/view.proto` 语法有效。
- `rg` 检查结果显示：
  - 现有 `raft.proto` / `metadata.proto` 使用 `package raft;`
  - 现有 `storage_node.proto` 使用 `package storage;`
  - 新增 `view.proto` 使用独立 `package view;`
  - 各文件都保持了“独立 package + 对应 service + 对应 ResponseSummary / StatusCode”的现有风格

## 结论

- T008 已完成。
- 当前可以进入 T009。
