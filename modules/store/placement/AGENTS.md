# modules/store/placement

## 模块职责

- 本模块只承接 StorageNode 副本候选节点的本地选择策略。
- 当前实现 `ReplicaPolicySelector::SelectReplicas()`、`PlacementManager::SelectPlacement()` 的静态 candidates 路径，以及生产 `StorageNodeRegistry` snapshot -> placement eligibility 的 manager 协调入口。

## 修改规则

- 保持命名空间为 `storedemo`。
- 这里只做纯策略计算，不做文件 IO、RPC、metadata commit 或 Raft 交互。
- 不要在这里调用 `StorageNodeClient::WriteChunk`、`RaftNode::ProposeMetadata()`、`MetadataStateMachine`。
- 不要把对象 payload、文件路径或本地 chunk 内容引入 placement 决策结构。
- `PlacementManager` 负责协调静态 candidates 或生产 registry snapshot 输入、补充决策说明并复用 `ReplicaPolicySelector`，不要在 manager 层复制一套筛选排序逻辑。
- registry snapshot 路径只允许做 liveness / facts 完整性这类保守过滤；健康、磁盘压力、容量阈值、写过载和稳定排序仍应交给 `ReplicaPolicySelector`。
- 排序、排除和选择语义要保持确定性和可测试性。
