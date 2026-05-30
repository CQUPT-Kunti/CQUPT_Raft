# modules/store/upload

## 模块职责

- 本模块承接 upload coordinator / helper。
- 当前只实现最小 `UploadCoordinator::UploadObject()`。

## 修改规则

- 保持命名空间为 `storedemo`。
- 这里是协调层，不做 Raft 提案、不直接调用 `MetadataStateMachine`、不直接写本地 chunk 文件。
- 通过抽象 `UploadMetadataClient` 和 `UploadChunkWriter` 串联 metadata/control-plane 与 StorageNode/data-plane。
- 不要在这里实现 GC、Repair、Rebalance、Scrub、heartbeat 或 registry。
- 不要把 payload 写入 metadata、Raft log、snapshot 或 state machine。
