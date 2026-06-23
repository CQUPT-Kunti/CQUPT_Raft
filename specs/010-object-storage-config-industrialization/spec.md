# Feature Specification: Object Storage Config Industrialization

**Feature Branch**: `010-object-storage-config-industrialization`  
**Created**: 2026-06-18  
**Status**: Draft  
**Input**: User description: "对 object storage / store 的大文件上传、chunk placement、metadata manifest、repair 方向、并发执行和 runtime 默认策略做一次工业化规划，第一阶段快速得到正确、可测、可扩展版本。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Per-Chunk Dynamic Placement (Priority: P1)

作为对象存储写入方，上传一个由多个 chunk 组成的大对象时，每个 chunk 都应得到独立、资源感知的 replica placement，且上传端只能执行 write plan，不能按 `node_id`、节点名称或配置顺序自行挑选副本。

**Why this priority**: 这是从“能跑”到“工业化对象存储”的最小正确性边界。没有 per-chunk dynamic placement，后续并发、manifest、read fallback 和 repair 都会继续受固定组或固定顺序限制。

**Independent Test**: 使用 9 个健康 StorageNode、相同 replica policy 和多个 chunk 的对象，验证不同 chunk 可以得到不同 replica set，分布覆盖大多数健康节点，且打乱 `node_id`、节点名称或配置顺序不会改变资源等价条件下的语义优先级。

**Acceptance Scenarios**:

1. **Given** 9 个健康、可写、容量充足的 StorageNode，**When** 为 8 个以上 chunk 创建 write plan，**Then** 每个 chunk 都有独立的 replica set，整体副本覆盖大多数健康节点。
2. **Given** 相同资源事实但 `node_id`、节点名称和配置顺序被打乱，**When** 重复生成 write plan，**Then** placement 不出现按字典序或配置顺序取前 N 个节点的行为。
3. **Given** 部分节点处于 draining、offline、degraded、高磁盘压力、容量不足或写入过载状态，**When** 生成 write plan，**Then** 这些节点不会进入新写入 selected replica set，并且拒绝原因可诊断。

---

### User Story 2 - Bounded High-Concurrency Upload (Priority: P1)

作为对象存储客户端，上传 GB 到百 GB 级对象时，系统应允许多个 chunk 同时 in-flight，同一个 chunk 的多个 replica target 并行 fan-out，同时用明确的 chunk 数、字节数、RPC 并发和 staging 风险边界保护资源。

**Why this priority**: 大文件上传不能继续停留在保守串行模型，但高并发必须 bounded，否则会把内存、磁盘、RPC 队列和 fsync 压力转成新的生产风险。

**Independent Test**: 使用受控 chunk reader 和阻塞 writer 验证 `max_inflight_chunks`、`max_inflight_bytes` 和 per-chunk replica fan-out 同时生效；当任一预算耗尽时，读取暂停并形成 backpressure，不会把整文件读入内存。

**Acceptance Scenarios**:

1. **Given** 上传请求配置了 `max_inflight_chunks=2` 和有限 `max_inflight_bytes`，**When** 上传包含多个 chunk 的对象，**Then** 同时 in-flight 的 chunk 数和 payload 字节数不超过预算。
2. **Given** 一个 chunk 需要写入 3 个副本且 `minimum_successful_writes=2`，**When** 其中 2 个副本 durable 成功、另一个副本慢或失败，**Then** chunk 可进入 commit-eligible 状态，失败副本被记录为 retry/degraded/cleanup 或 repair candidate 事实。
3. **Given** StorageNode RPC 返回 timeout、overloaded、node unavailable 或 checksum mismatch，**When** 上传执行 fan-out，**Then** 错误聚合能区分 retryable 和 non-retryable，且最终 CommitObject 只在所有必要 chunk durable 后发生。

---

### User Story 3 - Metadata-Driven Manifest Read And Repair-Ready Facts (Priority: P2)

作为对象读取方或后续 repair 组件，系统必须以 metadata manifest 为权威来源读取 chunk replica，并为“副本丢失后重新 placement，不强制补回原节点”的 repair 决策保留必要事实。

**Why this priority**: per-chunk dynamic placement 后，读取和 repair 都不能假设固定 replica group。manifest 必须准确保存 `chunk_index -> replica_nodes`，读取必须能在同一 chunk 的多个健康副本之间 fallback。

**Independent Test**: 构造 committed manifest，其中不同 chunk 拥有不同 replica set；让每个 chunk 的首选副本失败，验证读取会尝试同 chunk 的其他 manifest 副本并做 checksum 校验。

**Acceptance Scenarios**:

1. **Given** committed object manifest 包含每个 chunk 的 replica nodes，**When** 下载对象，**Then** 读取只使用 manifest 中该 chunk 的 replica set，不使用固定 group 或 discovery 推断结果。
2. **Given** 某个 manifest 副本读取失败或 checksum mismatch，**When** 同 chunk 仍有其他健康副本，**Then** 读取尝试其他副本并记录失败诊断。
3. **Given** 后续 repair 发现某个副本 missing，**When** 需要补副本，**Then** 设计允许重新调用 placement 选择当前更合适的新节点，不要求补回原节点。

---

### User Story 4 - Runtime Defaults And Cross-Platform Boundaries (Priority: P3)

作为运维和开发人员，系统应有清晰、单一的生产默认 chunk size 和最小 runtime tuning 入口，并保持跨平台可编译、可测试、无 Linux-only 业务路径。

**Why this priority**: runtime tuning 是工业化必要条件，但不应拖慢第一阶段。第一阶段只收紧默认值、边界和可测试入口，避免大范围硬件 profile 重构。

**Independent Test**: 验证生产默认 chunk size 只有一个代码级入口，上传配置不再从 cluster/config 文件读取 chunk size；构建路径不引入 Linux-only API，跨平台不需要静默降级。

**Acceptance Scenarios**:

1. **Given** 客户端配置文件包含或不包含 `chunk_size_bytes`，**When** 执行上传配置解析，**Then** 第一阶段只使用单一代码级生产默认 chunk size，不从配置文件读取该字段作为语义来源。
2. **Given** 并发默认参数未显式传入，**When** 上传开始，**Then** 系统使用安全的小默认值，并在诊断中暴露 effective concurrency、max in-flight chunks 和 max in-flight bytes。
3. **Given** 在非 Linux 平台编译，**When** 构建受影响模块，**Then** 共享业务路径只依赖标准 C++ 或已有跨平台封装，不出现散落的 Linux-only 依赖。

## Current Baseline & Scope Boundaries *(mandatory)*

### Existing Baseline

- 已有 `CreateWritePlan -> WriteChunk -> CommitObject -> Download` 的对象上传/下载闭环，且对象可见性边界仍由 final `CommitObject` 决定。
- `ChunkRef` / `ObjectRecord` 已能保存 committed manifest 的 chunk refs 和 `replica_nodes`，且不携带 payload bytes。
- `TransferWritePlan`、`TransferChunkPlan`、`TransferCommittedChunk`、`TransferCommittedManifest` 已作为 transfer 侧 metadata facts 边界存在。
- `PlacementManager` / `ReplicaPolicySelector` 已支持健康、容量、负载、磁盘压力、failure domain、最小成功副本数和读副本选择。
- `LocalDiskChunkStore` 已支持 chunk durable write/read/delete/stat/list、staging publish、index rebuild、checksum verify 和不同 chunk 并发写入。
- `StorageTransferClient` 已支持单 StorageNode `WriteChunk` / `ReadChunk` RPC 适配和有限 retry/backoff。
- `ObjectTransfer` upload 现状是两遍读取，第二遍仍为单 chunk 串行上传；download 已是 committed manifest driven，但当前生产下载只选择第一个可发现副本。

### Targeted Gaps Or Risks

- 当前生产上传仍可按 discovery target 的 `node_id` 排序 fallback，存在名称/配置顺序影响 placement 的风险。
- 当前 write plan 没有完整表达 per-chunk selected replica nodes，upload 执行层仍承担了过多 target 决策。
- 当前同一个 chunk 的 replica 写入是串行 fan-out，慢副本会拖累整个 chunk。
- 当前 session 级并发预算明确收紧为单 chunk in-flight，缺少 bounded multi-chunk pipeline。
- 当前 runtime 默认 chunk size 在客户端路径仍存在 4MiB 默认值和配置读取入口，和生产固定 chunk size 目标不一致。
- 当前 read path helper 已有 manifest replica fallback 能力，但生产 `ObjectTransfer` download 尚未按同 chunk 多副本 fallback。
- repair/restart 方向已有 manager 基础设施，但第一阶段只要求 manifest 和 placement 支持 repair 决策 B，不实现完整闭环。

### Non-Goals

- 不把 store 节点设计成 Raft group，不引入固定 replica group，不让一个对象所有 chunk 固定落到同一组节点。
- 不提前暴露未完成对象，不改变 final `CommitObject` 作为对象可见性边界。
- 不让 payload 进入 metadata、Raft log、Raft snapshot 或 metadata snapshot。
- 不改变持久化格式，不重写 metadata state machine，不重命名现有类/函数/命名空间。
- 不实现完整单遍流式 upload、完整 resumable upload session、完整 repair 决策 B 闭环、完整 runtime tuning 工业化或复杂 failure-domain 策略。
- 不生成大量 DTO/Plan/Context/Options/Result/Record 类型；第一阶段优先扩展现有 `TransferSession`、`TransferWritePlan`、manifest 和 placement policy 输入输出。

### Platform Scope

- Linux 是主要验证平台，使用现有 CMake/CTest/test.sh 入口。
- 第一阶段实现应只依赖标准 C++、gRPC/Protobuf、CMake、GoogleTest 和项目已有跨平台封装。
- 如必须使用平台 API，必须集中在隔离层并提供等价行为、明确错误或文档化较弱保证；业务路径不得散落 Linux-only 逻辑。

### Edge Cases

- 健康节点少于 `replica_count` 时，write plan 失败且不能 fallback 到 node_id 顺序。
- `minimum_successful_writes > replica_count`、`replica_count == 0` 或 chunk size 为 0 时，plan 必须明确失败。
- 节点资源事实相同或接近时，可以用 deterministic jitter 分散热点，但不能使用 `node_id` 字典序、节点名称或配置顺序作为生产优先级。
- 某个 chunk 达不到 `minimum_successful_writes` 时，不能调用 `CommitObject`，已 durable 副本必须进入 cleanup/degraded/repair candidate 事实。
- `CommitObject` 失败后，对象仍不可见，durable chunks 必须作为 orphan cleanup candidates 保留。
- 下载遇到首个 replica 失败、checksum mismatch 或 missing 时，应尝试同 chunk 的其他 manifest replica；所有副本失败后返回聚合错误。
- 空对象或最后一个小 chunk 仍必须满足 bounded reader、manifest layout 和 checksum 规则。
- 大文件上传不得让内存占用随文件大小线性增长。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST generate chunk-level dynamic write plans where each chunk independently selects `replica_count` StorageNode replicas.
- **FR-002**: System MUST NOT use `node_id`, node name, endpoint, or configuration order as production placement priority or silent fallback.
- **FR-003**: System MUST use resource-aware placement inputs including capacity, used ratio or available capacity, reserve capacity, disk pressure, health, write load, in-flight write bytes when available, failure domain, writable state, draining, degraded, and offline state.
- **FR-004**: System MUST provide deterministic placement behavior for tests, including deterministic jitter or equivalent tie-breaking that avoids fixed hotspots without relying on lexical node order.
- **FR-005**: CreateWritePlan MUST express object identity, upload session identity, chunk size, total chunks, replica count, minimum successful writes, placement epoch, plan expiry, and per-chunk selected replica nodes using existing response/manifest models wherever possible.
- **FR-006**: Upload execution MUST consume the returned write plan and MUST fail explicitly if required selected replica nodes are missing or not discoverable; it MUST NOT silently fall back to fixed node order.
- **FR-007**: Metadata manifest MUST persist the actual durable `chunk -> replica_nodes` facts accepted by `CommitObject`.
- **FR-008**: Same-chunk replica writes MUST fan out in parallel up to a bounded fan-out limit, and chunk success MUST depend on `minimum_successful_writes`.
- **FR-009**: Upload MUST support bounded multi-chunk in-flight execution with both `max_inflight_chunks` and `max_inflight_bytes` enforced.
- **FR-010**: Upload MUST apply backpressure before reading more payload when either in-flight chunk or byte budget is exhausted.
- **FR-011**: Upload MUST aggregate replica write errors with retryable/non-retryable classification and preserve cleanup/degraded/repair candidate facts for durable but uncommitted chunks.
- **FR-012**: Download MUST read from committed metadata manifest and try healthy same-chunk replicas before failing the chunk.
- **FR-013**: Download MUST verify chunk checksum and final object checksum before publishing the output file.
- **FR-014**: Chunk size MUST have one code-level production default and MUST NOT be read from cluster/config JSON in the first-stage upload path.
- **FR-015**: First-stage runtime defaults MUST provide safe bounded values for upload concurrency, fan-out concurrency, in-flight bytes, queue pressure, and diagnostics without auto-changing semantic durability parameters.
- **FR-016**: Repair direction MUST support decision B: missing replicas are repaired by re-running placement against current resource facts, not by forcing replacement on the original node.
- **FR-017**: First-stage implementation MUST add at most 1 to 2 core new types only if existing models cannot carry the required state; any such type must have a narrow lifecycle and documented necessity.
- **FR-018**: All first-stage changes MUST remain cross-platform by design and MUST isolate any platform-specific behavior behind existing project boundaries.

### Key Entities *(include if feature involves data)*

- **Write Plan**: Pending upload plan containing object/session identity, chunk sizing, total chunks, policy, placement epoch, expiry, and per-chunk selected replica nodes.
- **Chunk Plan**: Per-chunk placement facts containing chunk identity, offset, expected size/checksum, required replicas, minimum successful writes, and selected replica nodes.
- **Committed Manifest**: Metadata-authoritative committed object record containing ordered chunk refs and actual durable replica nodes per chunk.
- **Placement Candidate**: StorageNode fact set used for placement, including identity, endpoint, capacity, health, disk pressure, load, admission, and failure-domain facts.
- **Upload Concurrency Budget**: Effective per-session bounds for chunk count, payload bytes, fan-out work, and diagnostics. It should reuse existing session state unless a minimal helper becomes unavoidable.
- **Cleanup/Repair Candidate Fact**: Non-authoritative fact describing durable but uncommitted or degraded chunk replicas that require later GC or repair workflow.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With 9 healthy StorageNodes and at least 8 chunks, placement covers at least 6 distinct healthy nodes while each chunk still has exactly `replica_count` selected replicas.
- **SC-002**: Reordering candidate configuration, renaming nodes, or changing lexical `node_id` order does not produce a fixed "first N nodes" selection pattern.
- **SC-003**: At least two chunks in the same multi-chunk object can receive different replica sets under identical healthy-node conditions.
- **SC-004**: Upload fails with a clear error when CreateWritePlan lacks selected replica nodes or when selected nodes cannot be discovered; no silent fallback to node order occurs.
- **SC-005**: Committed manifest records the actual durable replica nodes for every chunk and read path uses those manifest nodes.
- **SC-006**: In a controlled fan-out test, multiple replicas of the same chunk are observed in parallel and the chunk completes after `minimum_successful_writes` durable successes.
- **SC-007**: In a controlled multi-chunk upload test, in-flight chunks and bytes never exceed configured bounds, and the reader pauses under backpressure.
- **SC-008**: Upload memory usage is bounded by configured in-flight bytes plus small metadata/checksum facts, not by total object size.
- **SC-009**: Production upload no longer has multiple hard-coded chunk size defaults or config-driven `chunk_size_bytes` semantic overrides.
- **SC-010**: Cross-platform build paths for affected modules introduce no Linux-only API in shared business logic.

## Assumptions

- 第一阶段允许保留当前两遍读取模型；单遍流式 upload 是重要问题，但会推迟到后续阶段，避免同时重写 checksum、write plan 和 commit 依赖。
- 第一阶段优先在 transfer/upload/placement/read path 中建立正确 plan 和 bounded 并发，不实现完整后台 repair 闭环。
- `replica_count`、`minimum_successful_writes`、durability contract、commit visibility boundary 和 consistency policy 是语义层参数，不由 runtime tuning 自动调整。
- `ChunkRef.replica_nodes` 是第一阶段 committed manifest 的权威副本记录入口，不新增 payload 字段。
- `node_id` 仍作为身份用于 manifest、RPC 定位、read、repair、日志和指标；它不能作为生产调度权重。
- 如果实现中必须新增核心类型，第一阶段总量最多 1 到 2 个，并优先放在 `.cpp` 内部或现有模块窄接口中。
