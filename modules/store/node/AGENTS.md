# modules/store/node

## 模块职责

- 本模块承接 StorageNode data-plane 的 RPC 适配层，以及 heartbeat / capacity / health / load 事实的 in-memory `StorageNodeRegistry`。
- 当前已实现 write/read/delete/heartbeat/report/register 的 service 适配、write/read/delete/heartbeat/report/register 的 client 适配，以及生产 `StorageNodeRegistry`；Placement 和 read replica selection 的真实 facts 消费仍未接线。

## 修改规则

- 继续保持命名空间为 `storedemo`。
- `StorageNodeRegistry` 只保存 data-plane facts 和 liveness，不接 metadata、Raft、PlacementManager 或 read replica selection 的真实接线。
- gRPC service / client 只做 proto 与本地 `storedemo` 请求/响应语义之间的字段和状态映射。
- heartbeat/report/register service 只能调用注入的 `StorageNodeRegistry`，不能在 node 层复制一套 sequence/stale/merge/liveness 业务逻辑。
- heartbeat/report/register client 当前只把 `timeout_ms` 映射到 `grpc::ClientContext` deadline；T061 既有 proto 没有这些 RPC 的 `best_effort_cancel` 字段，因此不能伪装成已有端到端取消透传。
- 不要在这里扩展 upload coordinator、metadata commit、Placement、Repair 或后续 read/delete/list 服务面。
- 不要调用 `RaftNode::ProposeMetadata()`、`MetadataStateMachine` 或 metadata service。
- 不要把 object payload 写入 Raft log、snapshot 或 metadata 状态机。
- timeout / cancellation 当前只能明确表达边界，不能伪装成已经具备运行中取消传播。
