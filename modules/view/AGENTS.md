# modules/view

一句话说明：`modules/view/` 是 008 阶段规划的 ViewNode 服务发现与状态观测模块，只提供 discovery-only / observation-only 能力。

## 修改前必读

- 先读根 `AGENTS.md`，再读本文件。
- 修改 ViewNode 关键结构、字段、状态枚举、liveness 超时策略、冲突诊断规则或 discovery 输出时，必须同步更新 `modules/view/module-notes.md`。
- 本目录不写任务执行流水账；任务报告写入对应 feature 的 `task-reports/`。

## 职责边界

允许在本目录实现或维护：

- ViewNode、MetadataNode、StorageNode 的注册记录。
- heartbeat sequence、last heartbeat、health、load、capacity、failure domain 等观测事实。
- `LIVE`、`STALE`、`SUSPECT`、`DEAD` 等 liveness 观测状态。
- `DiscoverMetadata`、`DiscoverStorage`、`GetClusterView` 等服务发现和集群视图能力。
- MetadataNode 的 leader hint 展示，以及 `REGISTERED`、`JOINING`、`LEARNER`、`VOTER`、`DOWN` 等观测状态。
- 同 endpoint 不同 node_id、同 node_id 不兼容注册、过期 heartbeat 等冲突和诊断信息。

必须保持：

- discovery-only：ViewNode 只返回可诊断的节点和端点候选。
- observation-only：ViewNode 只展示自己收到或推导出的观测事实。
- 可测试：注册、心跳、liveness、发现快照和冲突处理必须能用确定性时间源或显式输入验证。
- 可诊断：错误和冲突应包含 request_id、node_id、endpoint、sequence、观测状态等足够定位的信息。

## 禁止事项

- 不允许把 ViewNode 写成 Raft membership authority。
- 不允许让 ViewNode 保存 object manifest 的一致性权威副本。
- 不允许让 ViewNode 参与 `CommitObject` 决策。
- 不允许让 ViewNode 直接操作 StorageNode chunk 数据或 chunk publish/delete 流程。
- 不允许让 ViewNode 修改 Raft membership、降低 Raft quorum 或参与 Raft leader election。
- 不允许把新注册 MetadataNode 直接变成 voter。
- 不允许让 ViewNode 决定对象是否 `COMMITTED` 可见。

## 工程要求

- 保持 metadata/control-plane 与 StorageNode data-plane 边界清晰，ViewNode 不接收真实 object payload 或 chunk payload。
- 高并发路径应优先使用快照式读取和小粒度锁设计，避免发现查询阻塞心跳处理。
- 跨平台实现不得依赖 Linux-only 路径语义；如后续加入持久化 registry 或 lease 状态，必须先写清 durability contract。
- `.h` 只放接口、类型和轻量 inline；注册、心跳、超时转换、冲突诊断等复杂逻辑放在 `.cpp`。
