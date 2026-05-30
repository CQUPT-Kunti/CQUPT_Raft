# modules/store/node

## 模块职责

- 本模块只承接 StorageNode data-plane 的 RPC 适配层。
- 当前已实现 `StorageNodeService::WriteChunk` 和 `StorageNodeClient::WriteChunk`。

## 修改规则

- 继续保持命名空间为 `storedemo`。
- gRPC service / client 只做 proto 与本地 `storedemo` 请求/响应语义之间的字段和状态映射。
- 不要在这里扩展 upload coordinator、metadata commit、Placement、Repair 或后续 read/delete/list 服务面。
- 不要调用 `RaftNode::ProposeMetadata()`、`MetadataStateMachine` 或 metadata service。
- 不要把 object payload 写入 Raft log、snapshot 或 metadata 状态机。
- timeout / cancellation 当前只能明确表达边界，不能伪装成已经具备运行中取消传播。
