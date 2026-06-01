# modules/store/maintenance

## 模块职责

- 本模块只承接 store data-plane 的后台维护任务基础设施。
- 当前首个落点是 `GarbageCollector` 的 task model、bounded queue、可注入删除 handler，以及最小 task persistence / restart resume。

## 修改规则

- 保持命名空间统一为 `storedemo`。
- 这里只做后台任务模型、状态机、队列、重试、统计、task persistence 和 restart resume。
- 不要在这里调用 `RaftNode::ProposeMetadata()`、`MetadataStateMachine` 或 metadata service。
- 不要把 object payload 写入 Raft log、snapshot 或 metadata 状态机。
- 删除执行必须通过注入 handler 间接完成，不要在 maintenance 层硬编码直连 `LocalDiskChunkStore` 或绕过后续 safety 边界。
- 即使 task 能持久化和恢复，恢复后的删除也仍必须重新经过 metadata-driven safety checker，不能绕过 live-manifest gate。
