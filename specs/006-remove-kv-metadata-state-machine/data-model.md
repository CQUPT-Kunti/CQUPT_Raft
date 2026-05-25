# Data Model: Metadata-Only Raft State Machine V2

## Design Goal

V2 数据模型不再服务 KV，也不再把 metadata 视为“附着在 KV demo 上的一种附加记录”。它直接表达对象存储元数据控制面的主模型，并满足以下约束：

- Raft 只复制 metadata command
- apply 严格按 log index 顺序推进
- 查询只读取当前 metadata 视图
- `request_id` 去重与对象状态变更必须原子更新
- snapshot / restart / catch-up / leader switch 后，已提交元数据视图保持一致

## Entity Overview

```text
BucketRecord (bucket_table)
  1 -- owns --> N ObjectRecord (object_table)
ObjectRecord
  1 -- references --> N ChunkRef
ObjectRecord
  N -- indexed by --> object_index[bucket_name]
RequestRecord (request_table)
  1 -- describes --> one logical write intent
TombstoneRecord (tombstone_table, optional but recommended)
  1 -- blocks --> stale create/commit after delete
```

## BucketRecord

**Purpose**: 表示 bucket 命名空间及其生命周期边界。

### Required Fields

| Field | Meaning |
|-------|---------|
| `bucket_name` | bucket 唯一名 |
| `bucket_epoch` | bucket 生命周期代次；删除后重建必须递增 |
| `create_request_id` | 创建 bucket 的幂等键 |
| `created_at_log_index` | bucket 首次生效 index |
| `created_at_term` | bucket 首次生效 term |
| `object_count` | 当前 bucket 下 active object 数 |

### Rules

- `CreateBucket` 只能在 bucket 当前不存在时成功。
- `DeleteBucket` 只能在 bucket 内没有 active object facts 时成功。
- bucket 删除后允许后续新建，但必须形成新的 `bucket_epoch`，以拒绝旧 lifecycle 的 stale object 请求。

## ObjectRecord

**Purpose**: 表示单个对象当前有效生命周期的元数据事实。

### Required Fields

| Field | Meaning |
|-------|---------|
| `bucket_name` | 所属 bucket |
| `object_key` | bucket 内对象键 |
| `bucket_epoch` | 对应 bucket 生命周期代次 |
| `object_epoch` | 对象生命周期代次；每次合法新建递增 |
| `state` | `PENDING` / `COMMITTED` / `DELETED` |
| `create_request_id` | 创建请求幂等键 |
| `commit_request_id` | commit 请求幂等键，可为空直到 commit |
| `delete_request_id` | delete 请求幂等键，可为空直到 delete |
| `manifest_summary` | 对象大小、chunk 数、checksum 等摘要 |
| `chunk_ref_ids` | 逻辑 chunk 引用集合 |
| `created_at_log_index` / `term` | 创建 apply 边界 |
| `committed_at_log_index` / `term` | 提交 apply 边界 |
| `deleted_at_log_index` / `term` | 删除 apply 边界 |

### Visibility Rules

- `PENDING`: `HeadObject` / `ListObjects` 不可见
- `COMMITTED`: 对外可见
- `DELETED`: 不可见，但删除事实必须可恢复

### Abort Rule

`AbortObject` 不引入第四个对外状态。它终止一个 `PENDING` lifecycle，并把“该 lifecycle 已终止”的事实保存在 `RequestRecord` 以及必要的 terminal markers 中，确保：

- 旧 `CommitObject` 不能把已 abort 的对象变成可见
- 后续新建可以在新的 `object_epoch` 上重新开始

## ChunkRef

**Purpose**: 表示对象元数据中的逻辑 chunk 引用，而不是实际数据块。

### Required Fields

| Field | Meaning |
|-------|---------|
| `chunk_ref_id` | 内部唯一引用 |
| `bucket_name` / `object_key` / `object_epoch` | 归属对象生命周期 |
| `ordinal` | chunk 顺序 |
| `declared_size` | 逻辑大小 |
| `checksum` | 逻辑校验摘要 |
| `placement_hints` | 模拟位置引用 |

### Rules

- `ChunkRef` 只表达 metadata，不包含真实 bytes。
- `placement_hints` 不要求 DataNode 存在。
- `ChunkRef` 顺序必须在 snapshot / replay 后保持稳定。

## RequestRecord

**Purpose**: 表示幂等、冲突检测和重试恢复所需的写请求事实。

### Required Fields

| Field | Meaning |
|-------|---------|
| `request_id` | 幂等键 |
| `operation` | `CreateBucket` / `DeleteBucket` / `CreateObject` / `CommitObject` / `AbortObject` / `DeleteObject` |
| `target_bucket` | 目标 bucket |
| `target_object` | 目标对象，可为空 |
| `target_bucket_epoch` | 命中的 bucket 代次 |
| `target_object_epoch` | 命中的对象代次，可为空 |
| `command_fingerprint` | 同 request_id 内容一致性摘要 |
| `result_code` | 首次逻辑结果 |
| `result_state` | 首次结果对应对象状态，可为空 |
| `result_log_index` / `term` | 首次生效边界 |

### Rules

- `request_id` 相同且 fingerprint 相同 => replay
- `request_id` 相同但 fingerprint 不同 => idempotency conflict
- `RequestRecord` 与对象状态更新必须在同一 apply 临界区完成
- `RequestRecord` 必须进入 snapshot/restart 恢复路径

## TombstoneRecord

**Purpose**: 保存删除事实，阻止 stale lifecycle 复活。

### Required Fields

| Field | Meaning |
|-------|---------|
| `bucket_name` | 所属 bucket |
| `object_key` | 删除对象 |
| `bucket_epoch` / `object_epoch` | 被删除生命周期 |
| `delete_request_id` | 删除请求幂等键 |
| `deleted_at_log_index` / `term` | 删除边界 |
| `last_visible_checksum` | 最后一次可见对象的摘要，可选 |
| `delete_info` | 删除诊断信息 |

### Rules

- tombstone 不对 `HeadObject` / `ListObjects` 可见
- tombstone 必须能阻止旧 create/commit request 复活已删除对象
- 当新的合法 `CreateObject` 重新开始一个生命周期时，必须进入新的 `object_epoch`，并与旧 tombstone 区分

## Index Structures

### bucket_table

- key: `bucket_name`
- value: `BucketRecord`

### object_table

- key: `bucket_name + '\0' + object_key`
- value: `ObjectRecord`

### object_index

- key: `bucket_name`
- value: ordered set / ordered map of committed object keys

要求：
- `ListObjects` 只读 `object_index + object_table`
- 插入/删除 committed object 时同步更新
- 不能通过遍历整个日志生成列表

### chunk_ref_index

- key: composite object identity
- value: ordered `ChunkRef` list

### request_table

- key: `request_id`
- value: `RequestRecord`

### tombstone_table

- key: composite object identity or terminal lifecycle key
- value: `TombstoneRecord`

## Snapshot V2 Shape

### MetadataSnapshotHeaderV2

| Field | Meaning |
|-------|---------|
| `magic` | metadata snapshot V2 magic |
| `version` | snapshot version |
| `last_applied_index` | 状态机内部最后已应用 index |
| `last_applied_term` | 状态机内部最后已应用 term |
| `bucket_count` | bucket 条目数 |
| `object_count` | object 条目数 |
| `chunk_ref_count` | chunk ref 条目数 |
| `request_count` | request 条目数 |
| `tombstone_count` | tombstone 条目数 |

### Consistency Rule

- `SnapshotStorage` 外层 meta 的 `last_included_index/term` 必须等于 `MetadataSnapshotHeaderV2.last_applied_index/term`
- `LoadSnapshot()` 必须显式校验这两个边界一致
- 不一致时必须返回明确错误，不允许 best-effort 继续

## Concurrency Model

### Write Path

- `RaftNode` 负责 ordered apply
- `MetadataStateMachine::Apply()` 使用 unique/write lock
- write lock 期间同时更新：
  - bucket/object tables
  - object index
  - chunk ref index
  - request table
  - tombstone table
  - `last_applied_index/term`

### Read Path

- `HeadObject()` / `ListObjects()` 使用 shared/read lock
- 只能读取已提交状态
- 不允许读取半更新的 index/table 组合

### Snapshot Path

- node 先持有 `apply_mu_` 阻止新的 apply
- state machine 再持有 shared/read lock，复制一致性快照视图到本地 buffer
- 释放读锁后再执行文件 I/O
- 因为 apply 被阻塞，复制阶段看到的是确定的单点状态；因为读锁共享，查询可以与复制并发

## Explicit Exclusions

- 不为真实对象数据建模
- 不为 DataNode、blob store、replica placement health 建模
- 不增加第四个对外对象状态
- 不把日志历史当作查询索引
