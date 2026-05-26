# Research: Storage Node Data Plane

**Feature**: `007-storage-node-data-plane`  
**Date**: 2026-05-25  
**Scope**: 为现有 metadata-only Raft control-plane 规划新增 StorageNode chunk/data-plane。只做 plan，不写生产代码。

## Decision 1: 先做 LocalDiskChunkStore MVP

**Decision**: 第一阶段先实现本地 `LocalDiskChunkStore`，覆盖 `WriteChunk`、`ReadChunk`、`DeleteChunk`、`StatChunk`、`ListChunks`、checksum、staging/publish、ChunkIndex rebuild、stale staging cleanup、partial write detection 和 corrupted quarantine。  
**Rationale**: 当前项目已有 metadata manifest，但没有真实 chunk bytes 落盘。先把单节点 durability、恢复和索引做扎实，能最小化网络、Placement、Repair/Rebalance 同时引入导致的定位难度。  
**Alternatives considered**:
- 先做完整 StorageNode RPC 和分布式副本：被拒绝，因为没有可靠本地 chunk store 时，RPC 成功不等于 data durable。
- 直接实现 Repair/Rebalance：被拒绝，因为它们依赖 chunk durability、checksum、index 和健康副本事实。
- 复用 Raft snapshot storage 保存 chunk：被拒绝，因为这会混淆 metadata persistence 和 object payload persistence。

## Decision 2: `chunk_id` MVP 使用 `object_id + version + chunk_index`

**Decision**: MVP 的 chunk identity 使用 `object_id + version + chunk_index`，例如逻辑形式 `{object_id}:{version}:{chunk_index}`。  
**Rationale**: 现有 metadata 已有 `object_id` 和 `version`，该组合能稳定对应一个对象版本的一个 chunk，便于幂等写、manifest 查询、GC 和 repair。它不要求全局内容寻址索引，也不会把 chunk store 退化成 KV。  
**Alternatives considered**:
- 使用 checksum 作为 chunk_id：被拒绝，因为会把 checksum 与内容寻址/去重绑定，提前引入引用计数、跨对象共享和 GC 复杂度。
- 使用随机 UUID：可行但不优先，因为它削弱了从 manifest 推导 chunk identity 的可诊断性。
- 使用 bucket/object_key/chunk_index：被拒绝，因为 object_key 可被删除后重用，且多版本扩展更容易冲突。

## Decision 3: checksum 第一阶段只做校验，不做内容寻址或全局去重

**Decision**: checksum 用于 write/read/scrub/repair/migration 校验；第一阶段不做 content-addressed storage、不做 dedup、不做跨对象引用计数。  
**Rationale**: 当前核心风险是 durability 和恢复，不是存储效率。内容寻址会要求全局 refcount、并发引用维护、GC 强一致边界和 metadata 格式扩展，过早引入会扩大 blast radius。  
**Alternatives considered**:
- 立即做内容寻址：被拒绝，因为会把 GC 和 repair 的正确性问题变复杂。
- 不做 checksum：被拒绝，因为 write/read/repair/migration/scrub 都需要 corruption detection。

## Decision 4: 默认 3 副本，最小 2 个成功写副本

**Decision**: `ReplicaPolicy` 默认规划为 3 副本、每个 chunk 至少 2 个 durable write 成功后才允许 metadata commit；后续支持配置化。  
**Rationale**: 3 副本是易理解、易测试的 MVP 策略；最小 2 成功能在单副本失败时继续完成上传，同时保留一个副本级容错余量。  
**Alternatives considered**:
- 1 副本：被拒绝，因为无法验证副本选择、fallback 和 repair 的核心目标。
- 2 副本 + 2 成功：可用性较差，任一节点失败会阻塞写入。
- erasure coding：被拒绝，超出 007 MVP 范围，后续可作为独立 feature。

## Decision 5: StorageNode heartbeat 独立于 Raft heartbeat

**Decision**: StorageNode capacity/health/load heartbeat 独立于 Raft leader election heartbeat。  
**Rationale**: Raft heartbeat 表达共识节点 liveness，不表达 chunk_count、available capacity、disk pressure、IO error、load、maintenance、drain 或 corruption facts。Placement、read replica selection、Repair 和 Rebalance 需要数据面事实，不能复用共识心跳假装健康。  
**Alternatives considered**:
- 复用 Raft heartbeat payload：被拒绝，因为会污染共识协议边界，并让非 Raft StorageNode 无法被表达。
- 只靠客户端探测：被拒绝，因为 Placement 需要主动、持续、可观察的全局 facts。

## Decision 6: Linux `fsync/fdatasync` 与 Windows `FlushFileBuffers` 分别定义

**Decision**: StorageNode durable file abstraction 必须明确 Linux 与 Windows 的持久化操作：Linux 使用 `fdatasync`/`fsync` 和 directory sync；Windows 使用 file handle + `FlushFileBuffers`，并单独处理 publish/replace 语义。  
**Rationale**: 两个平台的文件、目录、rename/replace 和 flush 语义不同。项目 constitution 禁止 required durability operations 静默降级，因此必须在 contract 中先定义差异和失败处理。  
**Alternatives considered**:
- 只用 `std::ofstream::flush`：被拒绝，因为它不保证落盘。
- 用现有 Raft storage helper 直接代替：被拒绝，因为 chunk data-plane 有 staging/index/quarantine/rebuild 语义，不能绑定 Raft snapshot catalog。

## Decision 7: atomic publish 必须单独处理 Windows/Linux 风险

**Decision**: chunk publish 以 staging -> final 的原子边界规划，但 Linux rename、Windows `MoveFileEx`/`ReplaceFile`、父目录同步和崩溃窗口必须分别验证。  
**Rationale**: “rename 成功”不等于断电后一定可恢复。Linux 需要父目录 sync；Windows replace 语义受 handle、sharing mode、目标存在与 flags 影响。publish 后如果目录项未持久化，restart index rebuild 可能看不到 final chunk 或看到不一致事实。  
**Alternatives considered**:
- 将 staging 文件直接当 final 文件：被拒绝，因为 partial write 会被误认为 live chunk。
- 不 sync parent directory：被拒绝，除非某平台 contract 明确记录较弱保证或返回 unsupported。

## Decision 8: GC 必须 metadata-driven

**Decision**: GarbageCollector 的删除边界必须由 metadata tombstone/DELETED、aborted/pending timeout 和 live manifest 保护共同决定；本地 orphan scan 不能单独决定删除 committed live chunk。  
**Rationale**: metadata 是 object state 和 chunk manifest 的 source of truth。StorageNode 只能看到本地文件事实，不知道对象是否 live、是否正在 repair/rebalance、是否有新 manifest 尚未观察到。  
**Alternatives considered**:
- StorageNode 本地扫描后立即删除 unknown chunk：被拒绝，因为可能误删 committed live chunk。
- 删除对象时同步阻塞删除所有 chunk：被拒绝，因为会把对象删除可见性绑定到慢 IO 和失败节点，且不利于重试。

## Decision 9: Repair/Rebalance 不放进 MVP 第一阶段一次性实现

**Decision**: RepairManager、ScrubManager、RebalanceManager 在 007 中规划契约、数据模型和测试矩阵，但实现顺序放在 LocalDiskChunkStore、StorageNode RPC、最小上传/读/删闭环之后。  
**Rationale**: Repair/Rebalance 依赖健康副本事实、durable copy、manifest coordination、idempotent task、progress tracking 和并发互斥。过早实现会放大并发和一致性风险。  
**Alternatives considered**:
- 与上传闭环一起实现：被拒绝，因为故障定位困难，且可能引入半迁移 manifest。
- 完全不规划：被拒绝，因为 data-plane 长期可靠性必须提前定义边界，避免后续模型不兼容。

## Decision 10: 不恢复 KV，也不把 StorageNode 做成 KV

**Decision**: StorageNode 是 chunk/data-plane，不是 KV store；007 不重新引入 `CommandType::kSet`、`CommandType::kDelete`、`KvStateMachine`、`KvService`、`raft_kv_client`、`DebugGetValue`、KV proto、KV target、KV fallback 或 KV regression-only path。  
**Rationale**: 006 的成果是 metadata-only control-plane。StorageNode 只负责 chunk bytes 的 durable write/read/delete 和本地事实上报，object 可见性仍由 metadata state 决定。把 chunk 写成 KV 会复活旧 demo 语义，并诱导对象 payload 进入 Raft 或旧测试断言。  
**Alternatives considered**:
- 用 KV key 存 chunk bytes：被拒绝，因为会违反 no-KV 和“不把 object payload 写入 Raft log”的硬约束。
- 保留 KV 作为 debug fallback：被拒绝，因为 fallback 会长期侵蚀主路径边界。

## Decision 11: MVP 上传协调者先用 client 或 integration coordinator

**Decision**: MVP 不引入复杂 gateway 产品角色；先允许 client 或 integration coordinator 执行 object split、Placement 调用、并发 `WriteChunk`、最小副本成功判断和 `CommitObject`。  
**Rationale**: 当前项目已有 metadata RPC，但没有 data-plane。先用集成协调器验证闭环，可以避免在 gateway、auth、multi-tenant、routing 等非核心问题上扩展范围。  
**Alternatives considered**:
- 立即实现完整 gateway：被拒绝，超出 007 的一致性与 durability 核心目标。
- 让 StorageNode 调用 CommitObject：被拒绝，因为 StorageNode 不决定 object commit。

## Decision 12: 007 不实现完整多版本/generation

**Decision**: 007 只保留 `version`/future `generation` 扩展点，不实现完整多版本对象语义。  
**Rationale**: 现有 metadata 已有 `object_id` 和 `version`。MVP 要先证明 committed-only 读、delete tombstone、chunk manifest 和 durability 闭环，完整多版本会影响冲突、列举、GC 和读一致性。  
**Alternatives considered**:
- 一次性实现多版本：被拒绝，因为会扩大 metadata 状态机和 proto 变更。
- 删除 version 字段：被拒绝，因为已有模型需要兼容，且 chunk_id MVP 会使用它。

## Decision 13: 单独沉淀跨平台 durability contract

**Decision**: 在 `contracts/cross-platform-durability-contract.md` 中单独记录 Linux/Windows chunk durability 要求，并在 `plan.md` 增加独立 cross-platform validation matrix。  
**Rationale**: 仅把跨平台内容散落在 Phase 2、Phase 11 和 quickstart 中不够醒目，后续 `/tasks` 容易漏掉 Windows-specific 行为。独立 contract 可以让 fsync/fdatasync、directory sync、FlushFileBuffers、MoveFileEx/ReplaceFile、Windows long path、UTF-8 path、disk full、permission denied、partial write、atomic publish、restart rebuild 等要求成为可追踪任务源。  
**Alternatives considered**:
- 只保留 Phase 2/Phase 11 描述：被拒绝，因为检查时不够显眼，也不便于转成任务。
- 新增 `validation-matrix.md`：本阶段允许写入范围未列该文件，因此改为写入允许的 `contracts/` 目录和 `plan.md`。
