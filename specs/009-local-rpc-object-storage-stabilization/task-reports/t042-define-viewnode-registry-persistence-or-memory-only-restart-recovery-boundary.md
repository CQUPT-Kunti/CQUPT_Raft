## Scope

- 收口当前 ViewNode registry 的 restart recovery boundary。
- 只记录当前实现是 registry persistence 还是 memory-only。
- 不实现新的 peer sync、registry persistence、Raft recovery 或测试逻辑。

## Files Changed

- `modules/view/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`

## Current Registry Recovery Boundary

- 当前 `ViewNodeRegistry` 是 memory-only observed registry。
- 当前持久化到本地磁盘的只有 ViewNode 自己的 `node.identity`。
- registry 中的 observed records、liveness、leader hint、冲突诊断、peer observed snapshot 不会跨 ViewNode 进程重启保留。

## Memory-Only Or Persistence Decision

- 结论：当前阶段明确采用 memory-only restart recovery boundary。
- 没有发现 registry snapshot 文件、registry durable publish、registry load/replay、或 crash/restart registry persistence 代码路径。
- 如果未来要做 registry persistence、snapshot versioning、incarnation-safe durable recovery，需要另开任务实现。

## Restart Behavior

- ViewNode 重启后会丢失：
  - 所有 ViewNode / MetadataNode / StorageNode 的内存 observed records
  - `incarnation_id` / `sequence` / `last_seen_unix_ms`
  - TTL/liveness 派生状态
  - leader hint、cluster view warnings、冲突诊断
  - 通过 peer sync RPC 导入但未重新收到的 peer observed snapshot
- ViewNode 重启后不会丢失：
  - 本地 `node.identity`
  - 本次新进程启动重新生成的 process incarnation 能力

## Interaction With Self Refresh / Heartbeat / Peer Sync

- `view_node_app.cpp` 当前已有 startup register + self refresh loop，因此本地 ViewNode 自身 observed state 可以在重启后先重新建立。
- MetadataNode / StorageNode 必须重新 register / heartbeat，ViewNode 才能重新看到它们的 observed facts。
- 当前已经有 `PullPeerViewSnapshot` / `PushPeerViewSnapshot` RPC contract / adapter。
- 但当前没有 peer sync background loop / retry / active-active runtime convergence，所以不能假设重启后会自动从 peer rehydrate registry。
- peer sync snapshot 只是 observed-state sync，不是强一致状态复制，也不是 registry persistence。

## Non-Goals

- 不把 ViewNode registry 写成强一致配置中心。
- 不把 ViewNode registry 写成 Raft recovery。
- 不把 ViewNode 写成 Metadata/Raft membership authority。
- 不让 ViewNode 决定 voter / learner membership。

## Validation Performed

- 执行了文件存在性检查：
  - `test -f modules/view/module-notes.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/module-notes.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t042-define-viewnode-registry-persistence-or-memory-only-restart-recovery-boundary.md`
- 未执行 build/test。
- 原因：T042 是文档收口任务；本任务优先验证文档边界和文件存在性。

## Remaining Risks / Follow-Ups

- Windows/macOS 上没有单独验证 ViewNode registry restart recovery 行为，当前只能标记 pending，不能写 PASS。
- peer sync runtime loop 仍待后续任务补齐；当前只有 RPC contract / adapter。
- registry persistence 仍是未来工作；当前 memory-only 边界必须在后续任务中继续保持清晰。

## Result

- 状态：PASS
- 当前已明确 ViewNode registry restart recovery boundary 为 memory-only。
- 可以在文件检查通过后标记 T042 完成，并进入后续任务。
