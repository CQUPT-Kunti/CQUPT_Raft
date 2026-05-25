# Data Model: Strong Consistency Metadata Layer

## Overview

本文件记录当前已经固定的 metadata 数据模型。当前阶段只覆盖 metadata control plane，不引入 `StorageNode`、`ChunkStore`、真实 chunk 文件、真实上传下载、chunk replication、repair、rebalance 或 S3 协议相关数据模型。

当前稳定边界如下：

- Raft 只复制 metadata command，不复制真实大文件 bytes。
- `payload` 只允许 metadata-only 小字段，最大 `4096` 字节。
- `MetadataRecordState` 只包含 `Pending`、`Committed`、`Deleted`。
- 只有 `Committed` 对 `HeadMetadataRecord` / `ListMetadataRecords` 可见。
- `Deleted` 必须保留 tombstone，防止旧 `create/commit` 复活对象。
- snapshot/restart 必须恢复 committed metadata、tombstone 和必要的 `replay_table`。

## Entity: MetadataManifest

**Purpose**: 表示对象的 metadata-only manifest。它描述的是模拟对象布局，而不是实际 chunk 数据。

| Field | Type | Required | Constraints / Meaning |
|-------|------|----------|-----------------------|
| `object_size` | uint64 | yes | 必须 `> 0` |
| `chunk_size` | uint64 | yes | 必须 `> 0` |
| `chunk_count` | uint64 | yes | 必须 `> 0`，且必须等于 `ceil(object_size / chunk_size)` |
| `checksum` | string | yes | 必须非空，且不能只包含空白字符 |
| `mock_locations` | list<string> | yes | 列表不能为空；每个 entry 必须非空且不能只包含空白字符 |

### Fixed Rules

- `object_size`、`chunk_size`、`chunk_count` 共同描述模拟 manifest 的一致性关系：
  - `expected_chunk_count = 1 + ((object_size - 1) / chunk_size)`
  - `chunk_count` 必须等于 `expected_chunk_count`
- `mock_locations` 当前只是 location reference：
  - 不检查真实节点是否存在
  - 不检查真实路径是否存在
  - 不触发任何本地文件 IO
- `MetadataManifest` 不承载真实 chunk bytes，也不表达 StorageNode 运行时状态。

## Entity: MetadataRecordState

| State | External Visibility | Meaning |
|-------|---------------------|---------|
| `Pending` | Head/List 不可见 | create 已成功 apply，但对象尚未 commit |
| `Committed` | Head/List 可见 | commit 已成功 apply，对象进入稳定可见状态 |
| `Deleted` | Head/List 不可见 | delete 已成功 apply，删除事实通过 tombstone 保留 |

### Fixed State Transitions

```text
NeverCreated --CreateMetadataRecord--> Pending
Pending --CommitMetadataRecord--> Committed
Committed --DeleteMetadataRecord--> Deleted

Pending --same create request_id replay--> Pending
Committed --same commit request_id replay--> Committed
Deleted --same delete request_id replay--> Deleted
```

### Invalid Transitions

- `NeverCreated -> Committed`: 不允许，缺少可提交的 `Pending`
- `NeverCreated -> Deleted`: 不允许，对外结果为 `NOT_FOUND`
- `Pending -> Deleted`: 不允许，当前阶段 delete pending 为 `STATE_CONFLICT`
- `Committed -> Pending`: 不允许，状态不得倒退
- `Deleted -> Pending/Committed`: 不允许，旧请求不能绕过 tombstone 复活对象

## Entity: MetadataRecord

**Purpose**: 表示某个 `object_key` 当前的元数据记录。Raft 复制的是对该记录的状态转换，而不是真实对象数据。

| Field | Type | Required | Constraints / Meaning |
|-------|------|----------|-----------------------|
| `object_key` | string | yes | 必须非空；当前阶段作为对象唯一标识 |
| `state` | MetadataRecordState | yes | 只允许 `Pending` / `Committed` / `Deleted` |
| `manifest` | MetadataManifest | yes | create 时写入并随记录保留 |
| `payload` | string | optional | metadata-only 小字段；最大 `4096` 字节；不承载真实大文件 bytes |
| `create_request_id` | string | yes after create | create 幂等键；必须非空 |
| `commit_request_id` | string | required when Committed | commit 幂等键；`Committed` 时必须存在 |
| `delete_request_id` | string | required when Deleted | delete 幂等键；`Deleted` 时必须存在 |
| `created_at_log_index` | uint64 | yes after create | create 首次生效的 apply log index |
| `committed_at_log_index` | uint64 | required when Committed | commit 首次生效的 apply log index |
| `deleted_at_log_index` | uint64 | required when Deleted | delete 首次生效的 apply log index |
| `commit_info` | string | optional | metadata-only 提交说明 |
| `delete_info` | string | optional | metadata-only 删除说明 |

### Fixed Rules

- `object_key` 必须非空。
- `create_request_id`、`commit_request_id`、`delete_request_id` 只用于幂等和恢复，不表示客户端会话状态。
- `payload` 只允许 metadata-only 附加信息，不能扩展成真实文件内容。
- `Pending` 记录可存在于内部状态和 snapshot 中，但不对 `Head/List` 可见。
- `Committed` 记录是 `Head/List` 唯一可见的记录状态。
- `Deleted` 记录不对 `Head/List` 可见；删除事实由 tombstone 和必要 replay 状态共同保留。

## Entity: IdempotencyEntry

**Purpose**: 表示 `replay_table` 中的一条幂等结果，用于支持 timeout、leader failover、restart 后的同 `request_id` 重试。

| Field | Type | Required | Constraints / Meaning |
|-------|------|----------|-----------------------|
| `request_id` | string | yes | 必须非空 |
| `operation` | enum | yes | 只允许 `create` / `commit` / `delete` |
| `object_key` | string | yes | 本条幂等记录对应的对象键 |
| `command_fingerprint` | string | yes | 请求内容摘要；用于区分 replay 与 conflict |
| `result_code` | enum/string | yes | 首次逻辑结果，例如 `OK`、`IDEMPOTENT_REPLAY`、`STATE_CONFLICT` 等 |
| `result_state` | MetadataRecordState | optional | 首次结果对应的状态 |
| `log_index` | uint64 | optional | 首次生效的 apply log index |
| `response_record` | MetadataRecord summary | optional | 重放给客户端所需的摘要结果 |

### replay_table Fixed Role

- `replay_table` 以 `request_id` 为索引保存幂等结果。
- 相同 `request_id` + 相同 `operation` + 相同 `object_key` + 相同 `command_fingerprint` 视为重放。
- 相同 `request_id` 但 `operation`、`object_key` 或 `command_fingerprint` 不一致，视为 `IDEMPOTENCY_CONFLICT`。
- delete 成功时，`MetadataRecord`、`Tombstone` 和 `IdempotencyEntry` 必须保持一致更新。
- `replay_table` 必须在 snapshot/restart 后恢复到足以继续处理 create/commit/delete 重试的状态。

## Entity: Tombstone

**Purpose**: 表示删除事实。它是内部恢复模型的一部分，不是外部可见对象。

| Field | Type | Required | Constraints / Meaning |
|-------|------|----------|-----------------------|
| `object_key` | string | yes | 被删除对象的键 |
| `delete_request_id` | string | yes | 首次成功 delete 的幂等键 |
| `deleted_at_log_index` | uint64 | yes | delete 首次生效的 apply log index |
| `previous_commit_request_id` | string | optional | 被删除对象最后一次 commit 请求标识 |
| `checksum` | string | optional | 被删除对象最后保留的 checksum 摘要 |
| `delete_info` | string | optional | 删除说明 |

### Tombstone Retention Policy

- tombstone 必须保留删除事实，不能在 delete 成功后立即物理移除。
- tombstone 不对 `Head/List` 暴露。
- tombstone 必须阻止以下行为：
  - 旧 create 请求重新创建同一 `object_key`
  - 旧 commit 请求把已删除对象重新变成 `Committed`
- deleted-again 的稳定语义是：
  - 相同 `delete_request_id` 再删，命中 replay
  - 不同 `delete_request_id` 再删，对外为 `NOT_FOUND`

## Recovery Model

### Snapshot Contents

metadata snapshot / restart 恢复模型至少必须覆盖：

- 当前仍需要保留的 `MetadataRecord`
- 所有 tombstone
- 处理 create/commit/delete 重试所需的 `replay_table`

### Snapshot / Restart Fixed Semantics

- `Committed` 记录恢复后仍对 `Head/List` 可见。
- `Pending` 记录即使被恢复，也仍只作为内部状态存在，对 `Head/List` 不可见。
- `Deleted` 记录与 tombstone 恢复后继续对 `Head/List` 不可见。
- tombstone 与 `replay_table` 恢复后，旧 `create/commit/delete` 请求的幂等与防复活语义必须保持不变。
- snapshot 使用独立 metadata snapshot 格式；不复用或修改 KV snapshot 格式。

## Current-Phase Exclusions

当前阶段明确不引入以下数据模型：

- `StorageNode`
- `ChunkStore`
- 真实 chunk 元数据目录
- 真实副本布局 / replica health
- repair / rebalance / GC 调度模型
- S3 bucket / object version / multipart upload 模型

这些能力如果后续进入范围，必须在新的 spec 或后续阶段单独建模，且不得破坏当前的 committed-only visibility、tombstone 保留和 request_id replay 语义。
