# API Design: Strong Consistency Metadata Layer

## Overview

本文件记录当前已经固定的 Metadata API 语义。范围只覆盖 metadata control plane，不覆盖真实大文件上传、下载、chunk 存储或 StorageNode/ChunkStore 数据面能力。

Metadata API 的稳定边界如下：

- `payload` 只允许承载 metadata-only 小字段，不承载真实大文件 bytes。
- `CreateMetadataRecord` 只创建 `Pending` 记录。
- `CommitMetadataRecord` 才让记录进入 `Committed` 并对 `Head/List` 可见。
- `DeleteMetadataRecord` 只删除 `Committed` 记录，并写入 tombstone。
- `HeadMetadataRecord` / `ListMetadataRecords` 只暴露 `Committed` 记录。
- follower 上的 `Head/List` 返回 `NOT_LEADER` 和 leader hint，不返回本地 stale metadata。

## Common Concepts

### Common Request Fields

| Field | Required | Applies To | Description |
|-------|----------|------------|-------------|
| `request_id` | writes yes | create/commit/delete | 客户端生成的幂等键；`NOT_LEADER` 或 `TIMEOUT` 后必须复用同一值重试 |
| `object_key` | yes except list | create/commit/delete/head | 用户可见对象标识 |
| `client_timestamp` | optional | all | 客户端观察时间，仅用于诊断，不参与一致性判定 |

### Common Response Fields

所有 RPC 都通过 `summary` 返回统一摘要；其中 `leader_id` / `leader_address` 实际位于 `summary.leader_hint`。

| Field | Meaning |
|-------|---------|
| `code` | Metadata API 结果码 |
| `message` | 稳定的可读诊断摘要；用于区分参数错误、leader redirect、幂等冲突、状态冲突等 |
| `request_id` | 写请求回显的幂等键；`Head/List` 不依赖该字段 |
| `object_key` | 本次响应对应的对象键；`List` 可为空 |
| `state` | 当前结果对应的记录状态；成功 create 为 `Pending`，成功 commit/head 为 `Committed`，成功 delete 为 `Deleted` |
| `term` | 响应节点在返回时观察到的当前 Raft term |
| `log_index` | 写请求首次生效的 apply log index；幂等重放回显首次生效索引；读请求通常为 `0` |
| `leader_id` | 已知 leader 节点 id；位于 `summary.leader_hint.leader_id` |
| `leader_address` | 已知 leader 地址；位于 `summary.leader_hint.leader_address` |

### Status Codes

| Code | Fixed Meaning | Typical Use |
|------|---------------|-------------|
| `OK` | 请求首次成功完成 | create 成功创建 `Pending`；commit 成功转为 `Committed`；delete 成功转为 `Deleted`；head/list 成功返回 committed-only 结果 |
| `NOT_LEADER` | 当前节点不是 leader，未提供本地最终结果 | 任意 RPC 打到 follower；`Head/List` 在 follower 上也必须返回该码并附带 leader hint |
| `INVALID_ARGUMENT` | 请求字段缺失、为空、格式非法或 payload/manifest 越界 | 空 `request_id`、空 `object_key`、非法 manifest、payload 超限 |
| `NOT_FOUND` | 目标对象当前不存在可见或可操作结果 | `Head` 查询 never-created / `Pending` / `Deleted`；`Commit` 找不到待提交 `Pending`；`Delete` 删除 unknown；`Delete` 对已 `Deleted` 对象使用不同 `request_id` 再删 |
| `IDEMPOTENT_REPLAY` | 相同 `request_id` + 相同操作语义 + 相同 fingerprint 的重复请求 | create/commit/delete 的安全重试；返回首次逻辑结果和首次生效索引 |
| `IDEMPOTENCY_CONFLICT` | 相同 `request_id` 对应了不同操作、不同对象或不同 fingerprint | 同一 `request_id` 被重复用于不同 create payload、不同 commit 目标或不同 delete 目标 |
| `STATE_CONFLICT` | 对象存在，但当前状态不允许该转换 | create 命中已有 `Committed` 或 tombstone；commit 目标已经 `Committed` 或已 `Deleted`；delete 目标仍是 `Pending` |
| `INTERNAL_ERROR` | 服务内部 apply、查询或恢复路径发生非预期失败 | 未归类的内部错误 |
| `TIMEOUT` | 客户端在截止时间内未确认最终写结果 | 写请求等待 proposal/apply 超时；客户端必须复用同一 `request_id` 重试 |

### Committed-Only Visibility

- `HeadMetadataRecord` 只返回最新状态为 `Committed` 的记录。
- `ListMetadataRecords` 只列出状态为 `Committed` 的记录。
- `Pending` 和 `Deleted` 都不对外暴露。
- tombstone 只用于阻止旧请求复活对象，不作为 `Head/List` 的可见记录返回。

### Leader Hint

- `NOT_LEADER` 响应应尽量携带 `leader_hint`。
- 客户端收到 `NOT_LEADER` 后应切换到 `leader_hint` 指向的节点，并保留原 `request_id` 重试写请求。
- follower 上的 `Head/List` 不允许先读取本地 metadata 状态再返回结果；必须直接返回 `NOT_LEADER`。

## CreateMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `request_id` | yes | create 请求幂等键 |
| `object_key` | yes | 新对象键 |
| `manifest.object_size` | yes | 模拟对象大小 |
| `manifest.chunk_size` | yes | 模拟 chunk 大小 |
| `manifest.chunk_count` | yes | 模拟 chunk 数量 |
| `manifest.checksum` | yes | 模拟 checksum |
| `manifest.mock_locations` | yes | 模拟 chunk 位置引用，不要求真实节点可达 |
| `payload` | optional | metadata-only 小型 payload |

### Fixed Semantics

- 成功时返回 `OK`，并创建 `Pending` 记录。
- create 成功后对象对 `Head/List` 仍不可见。
- 相同 `request_id`、相同 `object_key`、相同 create fingerprint 的重复请求返回 `IDEMPOTENT_REPLAY`。
- 相同 `request_id` 但 create 内容不同，返回 `IDEMPOTENCY_CONFLICT`。
- 同一 `object_key` 已存在 `Committed` 记录时，返回 `STATE_CONFLICT`。
- 同一 `object_key` 已存在 tombstone 时，返回 `STATE_CONFLICT`；当前阶段不允许绕过 tombstone 直接重建对象。

### Response

| Field | Meaning |
|-------|---------|
| `summary.code` | `OK` / `IDEMPOTENT_REPLAY` / `IDEMPOTENCY_CONFLICT` / `STATE_CONFLICT` 等 |
| `summary.request_id` | create 请求的 `request_id` |
| `summary.object_key` | create 的 `object_key` |
| `summary.state` | 成功或幂等成功时为 `Pending` |
| `summary.term` | 返回时观察到的 term |
| `summary.log_index` | create 首次生效的 log index |
| `record` | 成功或幂等成功时返回 `Pending` 记录摘要 |

## CommitMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `request_id` | yes | commit 请求幂等键 |
| `object_key` | yes | 待提交对象键 |
| `expected_create_request_id` | optional | 用于诊断或对齐 create 来源；当前响应语义不依赖该字段单独建模 |
| `commit_info` | optional | metadata-only 提交说明 |

### Fixed Semantics

- 仅允许 `Pending -> Committed`。
- 成功时返回 `OK`，并使对象对 `Head/List` 可见。
- 相同 `request_id`、相同 commit intent 的重复请求返回 `IDEMPOTENT_REPLAY`。
- 相同 `request_id` 但 commit 目标或语义不同，返回 `IDEMPOTENCY_CONFLICT`。
- 目标对象不存在可提交的 `Pending` 记录时返回 `NOT_FOUND`。
- 目标对象已经 `Committed` 或已经 `Deleted` 时返回 `STATE_CONFLICT`。

### Response

| Field | Meaning |
|-------|---------|
| `summary.code` | `OK` / `IDEMPOTENT_REPLAY` / `NOT_FOUND` / `IDEMPOTENCY_CONFLICT` / `STATE_CONFLICT` 等 |
| `summary.request_id` | commit 请求的 `request_id` |
| `summary.object_key` | commit 的 `object_key` |
| `summary.state` | 成功或幂等成功时为 `Committed` |
| `summary.term` | 返回时观察到的 term |
| `summary.log_index` | commit 首次生效的 log index |
| `record` | 成功或幂等成功时返回 `Committed` 记录 |

## DeleteMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `request_id` | yes | delete 请求幂等键 |
| `object_key` | yes | 删除目标 |
| `delete_info` | optional | metadata-only 删除说明 |

### Fixed Semantics

- 仅允许 `Committed -> Deleted`。
- 成功时返回 `OK`，并写入 tombstone。
- delete 成功后对象对 `Head/List` 不再可见。
- 相同 delete `request_id`、相同 delete intent 的重复请求返回 `IDEMPOTENT_REPLAY`。
- 相同 `request_id` 但 delete 目标或语义不同，返回 `IDEMPOTENCY_CONFLICT`。
- 删除 never-created 对象返回 `NOT_FOUND`。
- 删除 `Pending` 对象返回 `STATE_CONFLICT`。
- 对已 `Deleted` 对象再次删除时：
  - 如果 `request_id` 相同，返回 `IDEMPOTENT_REPLAY`；
  - 如果 `request_id` 不同，返回 `NOT_FOUND`。
- deleted-again 不会移除 tombstone，也不会让对象重新可见。

### Response

| Field | Meaning |
|-------|---------|
| `summary.code` | `OK` / `IDEMPOTENT_REPLAY` / `NOT_FOUND` / `IDEMPOTENCY_CONFLICT` / `STATE_CONFLICT` 等 |
| `summary.request_id` | delete 请求的 `request_id` |
| `summary.object_key` | delete 的 `object_key` |
| `summary.state` | 成功或幂等成功时为 `Deleted` |
| `summary.term` | 返回时观察到的 term |
| `summary.log_index` | delete 首次生效的 log index |

## HeadMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `object_key` | yes | 查询对象键 |

### Fixed Semantics

- 只返回最新状态为 `Committed` 的记录。
- `Pending`、`Deleted`、never-created 都返回 `NOT_FOUND`。
- follower 上调用必须返回 `NOT_LEADER` 和 leader hint。
- follower 不允许返回本地 stale metadata，也不允许把 `Pending` / `Deleted` 当作可见记录返回。

### Response

| Field | Meaning |
|-------|---------|
| `summary.code` | `OK` / `NOT_FOUND` / `NOT_LEADER` / `INVALID_ARGUMENT` |
| `found` | 仅在 `summary.code == OK` 时为 `true` |
| `record` | 仅在找到 committed record 时返回 |
| `summary.state` | 成功时为 `Committed`；未找到时不表示可见状态 |

## ListMetadataRecords

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `prefix` | optional | 协议预留字段 |
| `limit` | optional | 协议预留字段 |
| `page_token` | optional | 协议预留字段 |

### Fixed Semantics

- 只返回 `Committed` 记录。
- `Pending` 与 tombstone 不出现在列表中。
- follower 上调用必须返回 `NOT_LEADER` 和 leader hint。
- 当前阶段列表结果保持 committed-only 语义；不因为 follower 本地状态而返回 stale metadata。

### Response

| Field | Meaning |
|-------|---------|
| `summary.code` | `OK` / `NOT_LEADER` |
| `records` | committed-only `MetadataRecord` 列表 |
| `next_page_token` | 当前阶段通常为空 |

## API Boundary

- Metadata API 不改变 `RaftService` 语义。
- Metadata API 不复用 KV `Put/Get/Delete` 的业务语义。
- Metadata API 不承诺真实数据上传、下载、chunk 存储或 StorageNode 可达性检查。
- 未来数据面接入不得改变 committed-only visibility、tombstone 保护和幂等重放语义。
