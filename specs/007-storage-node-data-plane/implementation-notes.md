# 007 StorageNode Data-Plane Implementation Notes

## 1. 当前实现范围

`StorageNode` data-plane 已集中实现于 `modules/store/`，负责 chunk bytes 的写入、读取、校验、持久化发布、本地索引、隔离与后台维护。metadata/control-plane 仍停留在现有 metadata/Raft 体系中，只保存对象状态、chunk manifest 和调度事实，不直接保存对象 payload。

当前能力已覆盖上传、读取、删除/GC、heartbeat/registry/placement、recovery/durability、scrub/repair/rebalance 的主链路。其中：

- 上传、读取、删除/GC、registry/placement、recovery/durability、quarantine、scrub、repair data-plane copy flow 已具备生产实现。
- rebalance 已具备生产 task model，以及 `target durable -> verify -> manifest coordination callback -> source cleanup` 的最小占位执行顺序。
- metadata manifest coordination、repair/rebalance task persistence、read-side repair、rebalance 自动后台调度仍未实现；相关逻辑当前仅保留 contract、callback 或任务边界，不宣称完整生产闭环。

## 2. 已完成任务摘要

### Setup / Foundational

完成了 `modules/store/` 基础目录、chunk model、校验和、错误语义、durable file contract、局部索引与有界执行器，建立了 StorageNode data-plane 的最小生产骨架。

### US1 上传闭环

完成 `CreateObject -> placement -> WriteChunk -> CommitObject` 最小上传闭环，支持 chunk durable write、checksum/size facts 回填和 metadata pending/commit 边界约束。

### US2 读取闭环

完成 metadata first 的对象读取流程：先根据 manifest 获取 chunk refs，再从 StorageNode replicas 读取并拼接 payload，补齐坏块识别、quarantine 与基础副本回退边界。

### US3 删除与 GC

完成 delete marker、chunk 引用安全检查、GC 候选筛选与删除链路，确保 metadata 生命周期与 data-plane cleanup 保持解耦。

### US4 Heartbeat / Registry / Placement

完成 StorageNode heartbeat、registry facts、placement policy 与读写消费方接线，使 metadata/control-plane 可以基于节点与副本事实做 placement 和后续维护决策。

### US5 Recovery / Durability / Quarantine

完成 LocalDiskChunkStore 重启恢复、磁盘扫描重建、staging 清理、坏块隔离与 Linux 主链路 durability 验证；Windows 仍停留在 contract 已定义但未实机收口的状态。

### US6 Scrub / Repair / Rebalance

完成 `ScrubManager` 有界后台队列、`RepairManager` task model 与 `RepairChunk` copy flow、under-replicated detection -> repair task 生成、`RebalanceManager` task model，以及 rebalance 最小 copy/verify/coordination/cleanup 占位流程。metadata manifest coordination、task persistence、read-side repair 与自动后台 rebalance 仍是后续工作。

### Final Validation

完成 no-KV audit、storage concurrency、recovery/snapshot/catch-up、最终 Linux 单线程分段验证，以及 manifest/chunk ref compatibility 检查。当前 007 的 Linux 侧已可收口。

## 3. 核心边界

- Raft 只保存 metadata/control-plane，不保存对象 payload。
- `MetadataStateMachine` 不保存 chunk bytes；metadata snapshot 不保存 chunk payload。
- `ObjectRecord.chunks` / `ChunkRef` 只保存 `chunk_id`、offset、size、checksum、`replica_nodes` 等 manifest facts。
- `StorageNode` 负责真实 chunk bytes 的持久化、读取、校验、隔离与本地维护。
- `StorageNode` 不决定对象 committed/deleted 可见性；对象可见性仍由 metadata/control-plane 决定。
- 上传仍遵循 `metadata pending -> chunk durable -> metadata commit`。
- 读取仍遵循 `metadata first -> read StorageNode replicas`。
- no-KV 收口成果未回退；没有恢复 KV fallback、KV proto、KV state machine 或把 StorageNode 设计成 KV。
- 对象 payload 不写入 Raft log、snapshot、metadata snapshot 或 `MetadataStateMachine`。

## 4. 验证结果

- T090 `no_kv_surface_audit` PASS。
- T091 `storage-node-concurrency` PASS。
- T092 recovery/snapshot/catch-up 低并发验证 PASS。
- T093 最终 Linux 单线程分段验证 PASS。
- T095 manifest/chunk ref compatibility PASS。
- 当前可以确认 007 的 Linux 侧实现已收口。
- Windows 未执行最终验证，不能宣称 Windows PASS。

## 5. T093 特别说明

T093 过程中，最终 Linux 验证曾被 `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot` 阻塞。后续在清理 snapshot/runtime 残留后，采用分段单线程方式完成最终验证并恢复 PASS。

后续如果再次执行全量 Linux 回归，建议继续保留以下操作：

- 全量前清理 snapshot/runtime 残留目录。
- 在测试 `165` 开跑前，定点清理 `raft_snapshot_diagnosis_*` 运行目录。

该问题当前作为非阻塞提示保留，不阻塞本轮 Linux 收口。

## 6. 跨平台 contract 偏差

Linux 主链路已经完成验证。Windows 侧目前仍只能确认 contract 已定义，尚未完成最终实机验证，不能写成 PASS。仍待验证的重点包括：

- long path 与 UTF-8 path
- `FlushFileBuffers`
- `MoveFileEx`
- `ReplaceFile`
- directory durability
- sharing violation
- delete/rename 语义

因此 Windows 相关结论目前只能标记为 contract-only / deferred。

## 7. 剩余风险

- Windows 实机验证待完成。
- 真实 metadata manifest coordination / Raft 提交未实现。
- `RepairManager` / `RebalanceManager` persistence 未实现。
- read-side repair 未实现。
- metadata / registry facts 仍存在新鲜度风险。
- repair/rebalance 后 manifest 更新与 cleanup 仍存在一致性风险。
- timeout / cancellation 的运行中传播边界仍未完全收紧。
- T093 snapshot diagnosis 用例对运行目录/恢复时序敏感，后续全量回归建议保留预清理步骤。

## 8. 后续建议

- 执行 Windows final validation，并在真实文件语义下收口 durability / rename / delete 路径。
- 把 metadata manifest coordination 从 callback/占位流程接到真实 metadata/Raft 提交。
- 为 repair/rebalance task 增加持久化与重启恢复能力。
- 增加 read-side repair。
- 补更完整的 failure injection / crash matrix。
- 增加真实多节点 E2E 验证，覆盖 metadata/control-plane 与 StorageNode data-plane 的联动边界。
