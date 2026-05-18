# Data Model: Strong Consistency Metadata Layer

## Overview

本数据模型描述 KV demo 向强一致元数据层演进时需要的领域对象。它只覆盖 metadata control plane，不包含 StorageNode、真实 chunk bytes、文件 IO、chunk replication、纠删码、rebalance 或 S3 协议。

## Entity: MetadataRecord

**Purpose**: 表示一个对象的强一致元数据记录。Raft 复制的是对该记录的状态转换命令，而不是真实对象数据。

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `object_key` | string | yes | 用户可见对象标识，在当前阶段全局唯一 |
| `state` | MetadataRecordState | yes | Pending、Committed 或 Deleted |
| `object_size` | uint64 | yes | 模拟对象大小，仅用于 metadata 验证 |
| `chunk_size` | uint64 | yes | 模拟切片大小 |
| `chunk_count` | uint64 | yes | 模拟 chunk 数量 |
| `checksum` | string | yes | 模拟对象或 manifest 校验值 |
| `mock_locations` | list<string> | yes | 模拟 chunk/replica 位置引用，不要求真实节点存在 |
| `payload` | string | optional | metadata-only 模拟 payload，不允许表示真实大文件 bytes |
| `create_request_id` | string | yes after create | 创建请求幂等键 |
| `commit_request_id` | string | optional | 提交请求幂等键 |
| `delete_request_id` | string | optional | 删除请求幂等键 |
| `created_at_log_index` | uint64 | yes after apply | 创建命令被应用的日志索引 |
| `committed_at_log_index` | uint64 | required when Committed | 提交命令被应用的日志索引 |
| `deleted_at_log_index` | uint64 | required when Deleted | 删除命令被应用的日志索引 |
| `commit_info` | object/string | optional | 模拟提交说明，例如 manifest version 或 commit note |
| `delete_info` | object/string | optional | 模拟删除原因或 delete marker 信息 |

### Validation Rules

- `object_key` 必须非空，并符合后续实现定义的 key 长度与字符限制。
- `object_size` 可以为 0，但必须与业务定义的空对象语义一致。
- `chunk_size` 必须大于 0。
- `chunk_count` 必须大于 0，且应与 `object_size` / `chunk_size` 的模拟关系一致。
- `checksum` 必须非空；当前阶段不要求真实校验算法。
- `mock_locations` 必须可解析为位置引用列表，但不要求真实 StorageNode 存在。
- `payload` 必须有大小上限，避免真实大文件内容进入 Raft command。
- `state=Committed` 时必须有 `commit_request_id` 和 `committed_at_log_index`。
- `state=Deleted` 时必须保留 tombstone 所需字段，至少包括 `object_key`、`delete_request_id`、`deleted_at_log_index` 和必要幂等结果。

## Entity: MetadataRecordState

| State | External Visibility | Meaning |
|-------|---------------------|---------|
| `Pending` | Head/List 不可见 | CreateMetadataRecord 已被接受，但对象尚未提交 |
| `Committed` | Head/List 可见 | CommitMetadataRecord 已成功，元数据对客户端可见 |
| `Deleted` | Head/List 不可见 | DeleteMetadataRecord 已成功，删除事实以 tombstone 保留 |

### State Transitions

```text
NeverCreated --CreateMetadataRecord--> Pending
Pending --CommitMetadataRecord--> Committed
Committed --DeleteMetadataRecord--> Deleted
Deleted --retry same DeleteMetadataRecord--> Deleted
Pending --retry same CreateMetadataRecord--> Pending
Committed --retry same CommitMetadataRecord--> Committed
```

### Invalid Transitions

- `NeverCreated -> Committed`: 缺少 Pending 记录时不能提交。
- `NeverCreated -> Deleted`: 删除未知对象默认返回 not found 或明确的幂等结果。
- `Pending -> Deleted`: 当前阶段不定义取消上传；删除 Pending 记录应返回 state conflict，除非后续 feature 单独规划 abort。
- `Committed -> Pending`: committed metadata 不得状态倒退。
- `Deleted -> Pending/Committed`: tombstone 不得被旧请求复活；同 key 重新创建需要后续版本化或 generation 语义，本阶段不做。

## Entity: MetadataCommand

**Purpose**: 表示被 Raft 复制的元数据状态转换意图。

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `operation` | enum | yes | create、commit、delete |
| `request_id` | string | yes | 写请求幂等键 |
| `object_key` | string | yes | 目标对象键 |
| `record` | MetadataRecord payload | required for create | create 时携带模拟 manifest |
| `commit_info` | object/string | optional | commit 时携带模拟提交信息 |
| `delete_info` | object/string | optional | delete 时携带模拟删除信息 |

### Rules

- 只有 write operation 进入 Raft command：create、commit、delete。
- Head/List 是 read operation，不应生成新的 Raft metadata command，除非后续实现需要线性一致读屏障。
- `request_id` 相同且 command fingerprint 相同表示重试。
- `request_id` 相同但 command fingerprint 不同表示 idempotency conflict。

## Entity: IdempotencyEntry

**Purpose**: 记录某个 `request_id` 的首次逻辑结果，支持超时、leader failover、客户端重试和 restart 后幂等。

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `request_id` | string | yes | 客户端生成的幂等键 |
| `operation` | enum | yes | create、commit 或 delete |
| `object_key` | string | yes | 请求目标对象 |
| `command_fingerprint` | string | yes | 请求内容摘要，用于冲突判定 |
| `result_code` | enum/string | yes | 首次逻辑结果 |
| `result_state` | MetadataRecordState | optional | 成功后对应状态 |
| `log_index` | uint64 | optional | 成功 apply 的日志索引 |
| `response_record` | MetadataRecord summary | optional | 需要重放给客户端的结果摘要 |

### Rules

- 成功的 create/commit/delete 必须记录幂等结果。
- 明确失败是否记录需在实现任务中细化；建议记录 state conflict/idempotency conflict，避免重试行为漂移。
- 幂等表需要随 snapshot/restart 恢复到足以处理重试的状态。

## Entity: Tombstone

**Purpose**: 表示删除事实。Tombstone 是内部可恢复状态，不对 Head/List 暴露为对象。

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `object_key` | string | yes | 被删除对象 |
| `delete_request_id` | string | yes | 删除请求幂等键 |
| `deleted_at_log_index` | uint64 | yes | 删除应用位置 |
| `previous_commit_request_id` | string | optional | 被删除 committed record 的提交请求 |
| `checksum` | string | optional | 被删除对象的最后 checksum 摘要 |
| `delete_info` | object/string | optional | 删除原因或 delete marker 信息 |

### Rules

- Tombstone 必须从 Head/List 中隐藏。
- Tombstone 必须进入 snapshot/restart 恢复集合。
- Tombstone 必须参与幂等和旧请求冲突判定。

## Entity: MetadataClientSimulation

**Purpose**: 描述 Metadata Client 生成的模拟对象日志和验证动作。

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `request_id` | string | yes for writes | 幂等键，可由用户指定以模拟重试 |
| `operation` | enum | yes | create、commit、delete、head、list |
| `object_key` | string | required except list | 目标对象 |
| `object_size` | uint64 | create required | 模拟对象大小 |
| `chunk_size` | uint64 | create required | 模拟 chunk 大小 |
| `chunk_count` | uint64 | create required | 模拟 chunk 数量 |
| `checksum` | string | create required | 模拟 checksum |
| `mock_locations` | list<string> | create required | 模拟位置 |
| `payload` | string | optional | 小型 metadata-only payload |
| `expect_visible` | bool | optional | 读后写验证期望 |
| `retry_count` | uint32 | optional | 客户端模拟重试次数 |

## Relationships

- 一个 `MetadataRecord` 对应一个 `object_key` 的当前状态。
- 一个 `MetadataRecord` 可引用一个模拟 `ChunkManifest`，但不拥有真实 chunk bytes。
- 一个 `request_id` 对应一个 `IdempotencyEntry`。
- 一个 `Deleted` record 必须有一个可恢复 `Tombstone`。
- Metadata Client 只构造模拟 command 和验证请求，不拥有服务端状态。
