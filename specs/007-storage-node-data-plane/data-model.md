# Data Model: Storage Node Data Plane

**Feature**: `007-storage-node-data-plane`  
**Date**: 2026-05-25  
**Boundary**: 本文规划未来实体，不修改当前生产代码、proto 或持久化格式。所有 object payload bytes 都在 StorageNode data-plane，不能进入 Raft log 或 metadata snapshot。

## Global Relationships

- `MetadataStateMachine` 保存 bucket/object lifecycle、object state、request idempotency、tombstone 和 manifest。
- `ObjectRecord.chunks` 与 `chunk_ref_index_` 当前承担 ObjectManifest 元数据角色。
- `ChunkRef` 是 metadata/control-plane 与 StorageNode data-plane 的兼容边界：`chunk_id`、`offset`、`size`、`replica_nodes`、`checksum`。
- StorageNode 保存 chunk bytes、local ChunkIndex、quarantine facts、delete tombstone 和本地健康事实。
- StorageNode 不决定 object 是否 committed；object 可见性只看 metadata。
- GC/Repair/Rebalance 必须在 metadata manifest 边界内更新或清理，不能凭本地文件事实直接改变 object state。

## Entity: StorageNode

**Fields**

| Field | Description |
|-------|-------------|
| `node_id` | 全局唯一 StorageNode 身份，写入 `replica_nodes` |
| `endpoint` | StorageNodeService 地址 |
| `data_dir` | chunk root，本地文件路径 |
| `capacity` / `used` / `available` | 容量事实 |
| `chunk_count` | 本地已知 chunk 数 |
| `health` | `HEALTHY` / `DEGRADED` / `READ_ONLY` / `UNAVAILABLE` / `DRAINING` |
| `disk_pressure` | `LOW` / `MEDIUM` / `HIGH` / `FULL` |
| `io_error_count` | 最近窗口 IO error 计数 |
| `load` | IO queue、active reads/writes、latency、bandwidth 摘要 |
| `last_seen` | registry 观察到的最后心跳时间 |
| `failure_domain` | zone/rack/host 占位字段 |

**States**: registered、healthy、degraded、read_only、draining、unavailable、removed。  
**Lifecycle**: `RegisterStorageNode` -> heartbeat active -> placement candidate -> degraded/draining/removed。  
**Idempotency**: 重复注册相同 `node_id` + 相同 endpoint 返回已有注册；不同 endpoint 必须显式 conflict 或 require re-register token。  
**Metadata Relation**: metadata manifest 只记录 `replica_nodes`，不保存 StorageNode 本地路径。registry facts 供 Placement/Repair/Rebalance 使用。  
**Raft Boundary**: StorageNode 不写 object payload 到 Raft，不通过 Raft heartbeat 表达 capacity/load。

## Entity: Chunk

**Fields**

| Field | Description |
|-------|-------------|
| `chunk_id` | MVP: `object_id + version + chunk_index` |
| `object_id` | 来自 metadata object identity |
| `version` | 来自 metadata version，保留 generation 扩展点 |
| `chunk_index` | 对象切片序号 |
| `offset` | 对象内偏移 |
| `size` | chunk bytes 大小 |
| `checksum` | `ChunkChecksum` |
| `state` | `ChunkState` |
| `created_at` / `updated_at` | 本地时间事实 |
| `final_path` | StorageNode 本地 final 文件路径，不进入 metadata |

**States**: staging、live、deleting、deleted、quarantined、corrupted、missing。  
**Lifecycle**: staged write -> checksum verify -> durable publish -> live -> tombstone/deleting -> deleted 或 corrupted/quarantined。  
**Idempotency**: 相同 `chunk_id` + 相同 size/checksum/content identity 重复写返回 already_exists；size/checksum 不同返回 conflict。  
**Metadata Relation**: committed object 的 `ChunkRef` 引用 `chunk_id`、`offset`、`size`、`checksum` 和 `replica_nodes`。  
**Raft Boundary**: Chunk bytes 只在 StorageNode；Raft 只保存 manifest 字段。

## Entity: ChunkMetadata

**Fields**

| Field | Description |
|-------|-------------|
| `chunk_id` | 本地 chunk identity |
| `node_id` | 所属 StorageNode |
| `size` | durable size |
| `checksum` | durable checksum |
| `state` | 当前本地状态 |
| `write_request_id` | 幂等写请求身份 |
| `delete_request_id` | 幂等删除请求身份 |
| `created_at` / `published_at` / `deleted_at` | 生命周期时间 |
| `last_verified_at` | 最近 scrub/read 校验时间 |
| `last_error` | 最近错误分类 |
| `quarantine_reason` | quarantine/corrupted 原因 |

**States**: follows `ChunkState`。  
**Lifecycle**: 随 `LocalDiskChunkStore` 写入、publish、读取校验、删除、quarantine 更新。  
**Idempotency**: `write_request_id` / `delete_request_id` 可帮助重复请求返回一致结果。  
**Metadata Relation**: 与 `ChunkRef` 的 checksum/size 必须一致；不替代 metadata manifest。  
**Raft Boundary**: 本地事实可被 heartbeat/scrub 汇报，但 payload 和本地路径不进入 Raft log。

## Entity: ChunkState

**Values**

| Value | Meaning |
|-------|---------|
| `STAGING` | 正在写入 staging，不能读给客户端 |
| `LIVE` | final 文件 durable 且 checksum 正确，可作为副本 |
| `DELETING` | 已收到删除/tombstone，等待物理删除或重试 |
| `DELETED` | 本地已删除，重复 delete 幂等成功 |
| `QUARANTINED` | 文件存在但不可信，需隔离 |
| `CORRUPTED` | checksum mismatch 或扫描发现损坏 |
| `MISSING` | index 或 manifest 期望存在但本地不存在 |

**Lifecycle**: `STAGING -> LIVE -> DELETING -> DELETED`；任意读取/扫描校验失败可转 `CORRUPTED/QUARANTINED`。  
**Idempotency**: `LIVE` 写入相同内容幂等；`DELETED/MISSING` 删除幂等；`CORRUPTED` 读取不得返回 success。  
**Metadata Relation**: 只有 `LIVE` 且 checksum 匹配的 replica 可用于读/repair source。  
**Raft Boundary**: 状态事实可作为 repair/GC 输入，但不改变 object committed 可见性。

## Entity: ChunkReplica

**Fields**

| Field | Description |
|-------|-------------|
| `chunk_id` | chunk identity |
| `node_id` | replica 所在节点 |
| `size` | replica size |
| `checksum` | replica checksum |
| `state` | live/corrupted/missing/deleting 等 |
| `last_verified_at` | read/scrub 验证时间 |
| `last_read_at` | 热点与读选择输入 |
| `failure_count` | 最近失败次数 |
| `error_code` | 最近错误分类 |

**States**: healthy、stale、corrupted、missing、deleting、unavailable。  
**Lifecycle**: Placement 生成目标 -> `WriteChunk` durable -> manifest commit -> read/scrub/repair 更新健康事实 -> GC/Rebalance 迁移或删除。  
**Idempotency**: 同一 `chunk_id/node_id` 的重复 repair copy 如果 checksum 相同则幂等；不同 checksum conflict。  
**Metadata Relation**: 当前 `ChunkRef.replica_nodes` 只保存 node list；更丰富 replica facts 可先在 registry/repair tracking 中规划，未来若进入 proto 必须单独扩展。  
**Raft Boundary**: replica facts 不能包含 bytes，manifest 更新必须在新副本 durable 后。

## Entity: ChunkIndexEntry

**Fields**

| Field | Description |
|-------|-------------|
| `chunk_id` | key |
| `state` | `ChunkState` |
| `size` | file size |
| `checksum` | checksum |
| `final_path` | final path |
| `staging_path` | staging path, if any |
| `metadata_path` | local sidecar path, if used |
| `lock_shard` | sharded index / lock striping 所属 shard |
| `updated_at` | 最近更新时间 |

**States**: mirrors `ChunkState`。  
**Lifecycle**: startup scan rebuild -> writes update -> reads verify -> deletes mark -> GC cleanup -> quarantine update。  
**Idempotency**: rebuild 多次应得到相同 live/quarantine 结果；delete/update 在 per-chunk lock 内串行。  
**Metadata Relation**: index 是本地 facts，不是 source of truth；GC 使用 metadata manifest 确认安全边界。  
**Raft Boundary**: index 不进入 Raft snapshot。

## Entity: ChunkChecksum

**Fields**

| Field | Description |
|-------|-------------|
| `algorithm` | MVP 可选 `SHA256` 或项目选定算法 |
| `value` | encoded checksum |
| `size` | 校验覆盖的字节数 |
| `computed_at` | 计算时间 |

**States**: expected、computed、verified、mismatch、unknown。  
**Lifecycle**: write streaming 计算 -> publish 前校验 -> manifest commit 记录 -> read/scrub/repair/migration 复验。  
**Idempotency**: 相同 checksum/size 是写入幂等判断的一部分；mismatch 必须 conflict 或 corrupted，不可 silent success。  
**Metadata Relation**: `ChunkRef.checksum` 保存 expected checksum；StorageNode 返回 computed checksum。  
**Raft Boundary**: checksum 可进入 metadata manifest，chunk bytes 不进入。

## Entity: StorageNodeHeartbeat

**Fields**

| Field | Description |
|-------|-------------|
| `node_id` | StorageNode identity |
| `capacity` / `used` / `available` | capacity facts |
| `chunk_count` | 本地 chunk 数 |
| `health` | health state |
| `disk_pressure` | pressure level |
| `io_error_count` | IO errors |
| `load` | active/queued ops、latency、bandwidth |
| `last_seen` | registry 写入时间 |
| `node_liveness` | alive/suspect/dead |

**States**: fresh、stale、suspect、dead。  
**Lifecycle**: periodic report -> registry update -> placement/read/repair consume -> timeout 降级。  
**Idempotency**: 相同 or older sequence 可被丢弃；新 heartbeat 覆盖旧 facts。  
**Metadata Relation**: 不替代 Raft heartbeat；可作为 metadata/control-plane 的 placement 输入。  
**Raft Boundary**: 不参与 Raft election safety。

## Entity: PlacementDecision

**Fields**

| Field | Description |
|-------|-------------|
| `chunk_id` | planned chunk |
| `replica_nodes` | selected nodes |
| `required_replica_count` | default 3 |
| `minimum_successful_writes` | default 2 |
| `excluded_nodes` | unhealthy/full/overloaded/recently failed |
| `reasons` | capacity/health/load/failure-domain/hotspot decision notes |
| `decision_epoch` | registry/metadata view version placeholder |

**States**: proposed、accepted、partially_written、satisfied、failed、expired。  
**Lifecycle**: upload coordinator asks Placement -> selected nodes -> WriteChunk attempts -> success condition -> CommitObject manifest。  
**Idempotency**: 同一 upload request 可以重用同一 decision；过期后重新 placement。  
**Metadata Relation**: 只有满足最小成功写副本数后，`replica_nodes` 才写入 `ChunkRef` manifest。  
**Raft Boundary**: decision 本身不是 object payload。

## Entity: ReplicaPolicy

**Fields**

| Field | Description |
|-------|-------------|
| `replica_count` | default 3 |
| `minimum_successful_writes` | default 2 |
| `read_selection` | health/load/locality/failure fallback |
| `failure_handling` | failed/corrupted/missing replica marking |
| `under_replicated_threshold` | healthy replicas below target/minimum |
| `erasure_coding_enabled` | MVP false |

**States**: active、draining_override、maintenance_override。  
**Lifecycle**: cluster config -> Placement -> write success -> read selection -> repair detection。  
**Idempotency**: policy evaluation is pure for a fixed registry/manifest snapshot。  
**Metadata Relation**: controls when `CommitObject` is allowed and when repair is required。  
**Raft Boundary**: policy does not store bytes; future config persistence must be explicit。

## Entity: GarbageCollectionTask

**Fields**

| Field | Description |
|-------|-------------|
| `task_id` | idempotent task identity |
| `chunk_id` | target chunk |
| `object_id` / `version` | source object identity |
| `reason` | pending_timeout / abort / deleted_object / orphan |
| `metadata_boundary` | tombstone/applied index/view used for safety |
| `target_nodes` | nodes to delete |
| `state` | pending/running/succeeded/failed/retry_wait |
| `attempts` / `last_error` | retry facts |

**States**: pending、running、succeeded、failed、retry_wait、cancelled。  
**Lifecycle**: metadata event or scan -> safety check -> BatchDeleteChunks/DeleteChunk -> retry until done or cancelled。  
**Idempotency**: `task_id + chunk_id + node_id` 重复执行安全；missing/deleted 视为成功。  
**Metadata Relation**: 必须确认 chunk 不属于任何 committed live manifest。  
**Raft Boundary**: GC 不删除 metadata live manifest；只清理 data-plane bytes。

## Entity: RepairTask

**Fields**

| Field | Description |
|-------|-------------|
| `task_id` | idempotent repair identity |
| `chunk_id` | target chunk |
| `source_node` | verified healthy source |
| `target_node` | healthy target |
| `expected_checksum` / `size` | manifest expected facts |
| `reason` | missing / corrupted / under_replicated |
| `state` | pending/copying/verifying/updating_metadata/succeeded/failed |
| `progress` | bytes copied / percent |
| `attempts` / `last_error` | retry facts |

**States**: pending、copying、verifying、target_durable、updating_metadata、succeeded、failed、retry_wait。  
**Lifecycle**: scrub/read/registry detects issue -> choose source/target -> copy -> checksum verify -> target durable -> metadata coordination -> complete。  
**Idempotency**: repeated copy to same target with same checksum is success/already_exists；different checksum conflict。  
**Metadata Relation**: new replica can enter manifest only after target durable；corrupted source cannot be used。  
**Raft Boundary**: repair copies chunk bytes StorageNode-to-StorageNode，不通过 Raft log。

## Entity: RebalanceTask

**Fields**

| Field | Description |
|-------|-------------|
| `task_id` | idempotent rebalance identity |
| `chunk_id` | migrating chunk |
| `source_node` | existing healthy replica |
| `target_node` | selected target |
| `reason` | capacity / hotspot / new_node_join |
| `expected_checksum` / `size` | manifest expected facts |
| `state` | pending/copying/verifying/updating_manifest/source_cleanup/succeeded/failed |
| `progress` | bytes copied / percent |
| `attempts` / `last_error` | retry facts |

**States**: pending、copying、verifying、target_durable、updating_manifest、source_cleanup、succeeded、failed、retry_wait。  
**Lifecycle**: policy detects imbalance -> select chunk/source/target -> copy -> verify -> metadata coordination -> optional old source cleanup。  
**Idempotency**: repeated migration to same target with matching checksum is success/already_exists；source cleanup is idempotent and metadata-guarded。  
**Metadata Relation**: avoid half-migrated manifest；target durable before manifest update, source delete after manifest no longer requires it。  
**Raft Boundary**: migration bytes stay in StorageNode data-plane。

## Entity: ObjectManifest

**Fields**

| Field | Description |
|-------|-------------|
| `bucket` | bucket |
| `object_key` | object key |
| `object_id` | object identity |
| `version` | version |
| `size` | object size |
| `etag` | object checksum/etag |
| `chunks` | ordered list of `ChunkRef` |
| `state` | PENDING/COMMITTED/DELETED from `ObjectRecord` |

**States**: pending_manifest、committed_manifest、deleted_tombstone。  
**Lifecycle**: `CreateObject` creates pending record without live readable chunks -> chunk durable facts collected -> `CommitObject` writes chunks -> `DeleteObject` tombstone removes visibility -> GC cleanup。  
**Idempotency**: request_id fingerprint protects repeated create/commit/delete；same commit manifest returns same result, conflicting manifest rejected。  
**Metadata Relation**: 当前由 `ObjectRecord.chunks + chunk_ref_index_` 承担，不新增真实实体。  
**Raft Boundary**: manifest stores chunk refs and checksum only，不保存 payload。

## Compatibility With Existing ChunkRef

当前 `ChunkRef` 已满足 007 MVP 的最小 manifest 需求：

- `chunk_id`: 对应 `object_id + version + chunk_index`。
- `offset`: 支持按对象偏移排序读取。
- `size`: 支持 partial write 和 read range 校验。
- `replica_nodes`: 记录 durable 成功副本所在 StorageNode。
- `checksum`: 支持 write/read/scrub/repair/migration 校验。

不兼容或暂不表达的 facts：

- per-replica health、last_verified_at、failure_count、local path、quarantine reason 不进入当前 `ChunkRef`。
- richer replica facts 先由 StorageNode registry、ChunkIndex 和 Repair/Rebalance task 跟踪；未来若必须持久进入 metadata，需要单独 proto/metadata plan。
