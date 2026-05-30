# modules/store/placement

## 模块职责

- 本模块只承接 StorageNode 副本候选节点的本地选择策略。
- 当前只实现 `ReplicaPolicySelector::SelectReplicas()` 及其相关数据结构。

## 修改规则

- 保持命名空间为 `storedemo`。
- 这里只做纯策略计算，不做文件 IO、RPC、metadata commit 或 Raft 交互。
- 不要在这里调用 `StorageNodeClient::WriteChunk`、`RaftNode::ProposeMetadata()`、`MetadataStateMachine`。
- 不要把对象 payload、文件路径或本地 chunk 内容引入 placement 决策结构。
- 排序、排除和选择语义要保持确定性和可测试性。
