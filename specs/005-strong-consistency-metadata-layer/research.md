# Research: Strong Consistency Metadata Layer

## Decision: Raft 只复制元数据命令

**Rationale**: 元数据命令小、确定、可重放，适合进入 Raft 日志。真实大文件 bytes 应属于未来 StorageNode/ChunkStore 数据面，否则会放大日志、snapshot、catch-up 和 restart recovery 成本。

**Alternatives considered**: 把文件 payload 放入 KV value 或 metadata command；拒绝，因为违反本 feature 的明确非目标。

## Decision: 使用 Pending / Committed / Deleted 三态

**Rationale**: Create 和 Commit 分离可模拟上传记录与提交记录；Deleted tombstone 可支持删除恢复和旧请求冲突处理。

**Alternatives considered**: 只用存在/不存在；拒绝，因为无法表达 committed-only visibility 和 tombstone。

## Decision: request_id 幂等结果由服务端状态机负责

**Rationale**: 客户端重试、leader failover 和 restart 后仍需得到同一逻辑结果。幂等表如果只在客户端保存，不能处理多客户端或客户端重启。

**Alternatives considered**: 客户端本地去重；拒绝，因为不具备全局一致性。

## Decision: Head/List 只返回 Committed

**Rationale**: 用户可见对象存在性以 CommitMetadataRecord 为边界。Pending 表示上传或 manifest 尚未提交，不应被外部读取。

**Alternatives considered**: Head 返回 Pending 状态给客户端；拒绝，因为会让未提交对象被误认为存在。

## Decision: 当前阶段不定义 StorageNode 接口

**Rationale**: 当前目标是验证 metadata control plane，一旦引入 StorageNode 会扩大到数据面、传输、放置和复制策略。

**Alternatives considered**: 同时规划 StorageNode stub；拒绝，因为会模糊本阶段边界并增加无关任务。
