# Data Model: Object Storage Config Industrialization

## Existing Entities Reused

### `TransferSession`

- **Purpose**: 单次 upload/download 生命周期快照。
- **Stage 1 Use**: 继续记录 direction、stage、request、chunk size、effective concurrency、progress、commit attempted、committed visible 和 failure。
- **Required Extension**: 可增加 in-flight budget 诊断字段或通过 diagnostics 输出，不引入新的 session 类型。
- **Lifecycle**: 客户端一次 transfer 开始时创建，完成/失败时结束。

### `TransferWritePlan`

- **Purpose**: CreateWritePlan 返回的 metadata/placement facts。
- **Stage 1 Use**: 承载 object identity、version、object checksum、per-chunk plans、created/expires time。
- **Required Extension**: 表达 `chunk_size_bytes`、`total_chunks`、`replica_count`、`minimum_successful_writes`、`placement_epoch`、`plan_expire_at` 和 per-chunk selected replica nodes。
- **Lifecycle**: pending object 创建成功后生成，upload 执行期间只读消费，CommitObject 后不再作为权威。

### `TransferChunkPlan`

- **Purpose**: 单 chunk 的 write plan facts。
- **Stage 1 Use**: 承载 chunk identity、offset、expected size/checksum、required replica count、minimum successful writes、selected replica nodes 和可选 candidate nodes。
- **Required Extension**: 若现有 `candidate_nodes` 语义不足，增加 selected replica nodes 字段；不新增平行 Plan/DTO 类型。
- **Lifecycle**: 属于 `TransferWritePlan`，在 upload session 内消费。

### `TransferCommittedChunk`

- **Purpose**: CommitObject 和 committed manifest 使用的 durable chunk facts。
- **Stage 1 Use**: 只记录达到 minimum_successful_writes 的实际 durable replica nodes。
- **Lifecycle**: chunk fan-out 完成后生成，CommitObject 成功后成为 manifest facts，失败时成为 cleanup candidate 来源。

### `ChunkRef`

- **Purpose**: metadata-authoritative manifest chunk record。
- **Stage 1 Use**: 保存 `chunk_id`、`offset`、`size`、`replica_nodes`、`checksum`，不保存 payload。
- **Lifecycle**: CommitObject 成功后进入 metadata state machine 和 snapshot。

### `StorageNodePlacementCandidate`

- **Purpose**: placement 的资源、健康、负载和 failure-domain 输入。
- **Stage 1 Use**: 继续作为 resource-aware placement 输入，不把 `node_id` 作为调度权重。
- **Lifecycle**: 从当前 StorageNode facts/snapshot 构造，单次 placement 决策结束后丢弃。

### `PlacementDecision`

- **Purpose**: 选出的 replica nodes、minimum_successful_writes、排除原因和 decision epoch。
- **Stage 1 Use**: 每个 chunk 独立生成，并映射到 write plan selected replica nodes。
- **Lifecycle**: 单次 chunk placement 决策结束后转换进 write plan。

### `CleanupCandidate`

- **Purpose**: durable but uncommitted 或 pending/deleted object chunk 的后续清理事实。
- **Stage 1 Use**: upload 失败或 CommitObject 失败时保留 cleanup risk，不作为 committed object 可见性依据。
- **Lifecycle**: transfer 失败结果返回，后续 GC/maintenance 消费。

## New Type Budget

Stage 1 的目标是 **0 个新增核心公开 struct**。

允许的例外只有一个：如果 parallel fan-out 和 multi-chunk pipeline 需要聚合 future/task 结果，可以在 `object_transfer.cpp` 匿名 namespace 中新增一个内部 helper 类型。该类型必须满足：

- 只聚合已有 chunk identity、target、write result、payload size、retryable 标记。
- 生命周期在一个 upload session 内结束。
- 不出现在 public header。
- 不增加跨模块调用链复杂度。

## State Transitions

### Upload Object

```text
Preparing checksums
-> Discovering metadata/storage facts
-> Planning per-chunk placement
-> Uploading chunks with bounded multi-chunk concurrency
-> Per-chunk fan-out durable success >= minimum_successful_writes
-> CommitObject
-> COMMITTED visible
```

Failure before CommitObject keeps object invisible and produces cleanup/degraded facts when durable chunks may exist.

### Chunk Upload

```text
Planned
-> In-flight
-> DurableSuccess on enough replicas
-> CommitEligible
-> IncludedInCommit
```

If durable successes are insufficient, chunk becomes failed and any durable partial replicas become cleanup/repair candidate facts.

### Read Chunk

```text
ManifestReplicaList
-> Ordered by read policy and observed facts
-> Try replica
-> Verify checksum
-> Success or try next replica
-> Fail only after all same-chunk replicas fail
```

### Repair Direction B

```text
Manifest says replica exists
-> Store/inventory/scrub detects missing or corrupted replica
-> Repair excludes current good replicas and unsuitable nodes
-> Placement selects new target from current facts
-> Copy and verify
-> Future manifest update replaces old missing replica with new durable replica
```

Full repair execution is deferred; Stage 1 only preserves the facts needed for this transition.
