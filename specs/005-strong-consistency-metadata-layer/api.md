# API Design: Strong Consistency Metadata Layer

## Overview

本文件规划 Metadata API 的语义契约。它不是当前阶段的源码变更，也不要求立即修改 `proto/raft.proto`。后续实现时可选择新增 `MetadataService`，或在兼容窗口内提供 metadata-specific CLI 到服务端适配层，但不能改变 Raft 内核协议语义。

## Common Concepts

### Common Request Fields

| Field | Required | Applies To | Description |
|-------|----------|------------|-------------|
| `request_id` | writes yes | create/commit/delete | 客户端生成的幂等键 |
| `object_key` | yes except list | create/commit/delete/head | 用户可见对象标识 |
| `client_timestamp` | optional | all | 客户端观察时间，仅用于诊断，不参与一致性判定 |

### Common Response Fields

| Field | Description |
|-------|-------------|
| `code` | 结果码 |
| `message` | 可读诊断信息 |
| `leader_id` | 当前已知 leader id |
| `leader_address` | 当前已知 leader 地址 |
| `term` | 响应时观察到的 Raft term |
| `log_index` | 写请求成功 apply 的日志索引；读请求可为空 |
| `request_id` | 写请求回显 |
| `state` | 相关记录状态 |

### Status Codes

| Code | Meaning | Retry Guidance |
|------|---------|----------------|
| `OK` | 请求成功 | 不需要重试 |
| `NOT_LEADER` | 当前节点不是 leader | 使用同一 `request_id` 发送到 leader hint |
| `INVALID_ARGUMENT` | 字段缺失、格式非法或 payload 超限 | 修正请求后使用新 `request_id` |
| `NOT_FOUND` | 目标 committed record 不存在或不可见 | 通常不重试，除非正在等待 commit |
| `IDEMPOTENT_REPLAY` | 相同 `request_id` 和相同意图的重复请求 | 视为成功或等价结果 |
| `IDEMPOTENCY_CONFLICT` | 相同 `request_id` 携带不同内容或操作 | 不应自动重试 |
| `STATE_CONFLICT` | 状态转换非法 | 不应自动重试，除非状态变化后重新发起新请求 |
| `INTERNAL_ERROR` | 内部 apply、snapshot 或未知失败 | 可保守重试同一 `request_id` 或转人工诊断 |
| `TIMEOUT` | 客户端未确认最终结果 | 必须使用同一 `request_id` 重试以查询最终结果 |

## CreateMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `request_id` | yes | create 请求幂等键 |
| `object_key` | yes | 新对象键 |
| `object_size` | yes | 模拟对象大小 |
| `chunk_size` | yes | 模拟 chunk 大小 |
| `chunk_count` | yes | 模拟 chunk 数量 |
| `checksum` | yes | 模拟 checksum |
| `mock_locations` | yes | 模拟 chunk 位置 |
| `payload` | optional | metadata-only 小型 payload |

### Semantics

- 成功后创建 `Pending` 记录。
- Pending 记录不通过 Head/List 可见。
- 同一 `request_id`、同一内容重复到达时返回 `IDEMPOTENT_REPLAY` 或等价成功结果。
- 同一 `request_id`、不同内容返回 `IDEMPOTENCY_CONFLICT`。
- 同一 `object_key` 已有 Committed 或 Deleted tombstone 时返回 `STATE_CONFLICT`，本阶段不做覆盖或版本化。

### Response

| Field | Description |
|-------|-------------|
| `state` | `Pending` |
| `log_index` | create command apply index |
| `record_summary` | object_key、manifest 摘要、checksum、request_id |

## CommitMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `request_id` | yes | commit 请求幂等键 |
| `object_key` | yes | 待提交对象键 |
| `expected_create_request_id` | recommended | 绑定对应 create，防止提交错误对象 |
| `commit_info` | optional | 模拟提交说明 |

### Semantics

- 仅允许 `Pending -> Committed`。
- 成功后记录对 Head/List 可见。
- 重复提交同一 `request_id` 和同一 intent 返回幂等结果。
- 缺少 Pending 记录返回 `NOT_FOUND` 或 `STATE_CONFLICT`，具体取决于是否存在 Deleted/Committed 历史状态。
- 对已 Committed 记录使用新的 commit request_id 默认返回 `STATE_CONFLICT`；使用相同 request_id 返回幂等结果。

### Response

| Field | Description |
|-------|-------------|
| `state` | `Committed` |
| `log_index` | commit command apply index |
| `record` | 提交后可见 MetadataRecord |

## DeleteMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `request_id` | yes | delete 请求幂等键 |
| `object_key` | yes | 删除目标 |
| `delete_info` | optional | 模拟删除原因 |

### Semantics

- 仅对 Committed record 执行 `Committed -> Deleted`。
- 成功后写入 tombstone，Head/List 不再返回该对象。
- 同一 delete request_id 重试返回幂等删除结果。
- 删除 never-created 或不可见对象返回 `NOT_FOUND`。
- 删除 Pending 对象返回 `STATE_CONFLICT`，本阶段不定义 abort upload。
- 删除已 Deleted 对象时，如果 request_id 相同则幂等成功；如果 request_id 不同则返回 `NOT_FOUND` 或 `STATE_CONFLICT`，实现阶段需固定一个对外结果。

### Response

| Field | Description |
|-------|-------------|
| `state` | `Deleted` |
| `log_index` | delete command apply index |
| `tombstone_summary` | object_key、delete_request_id、deleted_at_log_index |

## HeadMetadataRecord

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `object_key` | yes | 查询对象键 |

### Semantics

- 只返回最新状态为 Committed 的 record。
- Pending、Deleted、never-created 均返回 `NOT_FOUND`。
- 默认由 leader 提供读后写验证路径；follower read 或 lease read 不在当前阶段规划。

### Response

| Field | Description |
|-------|-------------|
| `found` | 是否找到 committed record |
| `record` | found=true 时返回完整 MetadataRecord |
| `state` | found=true 时为 `Committed` |

## ListMetadataRecords

### Request

| Field | Required | Description |
|-------|----------|-------------|
| `prefix` | optional | 预留前缀过滤；当前阶段可不实现 |
| `limit` | optional | 预留分页限制；当前阶段可不实现 |
| `page_token` | optional | 预留分页游标；当前阶段可不实现 |

### Semantics

- 只返回 Committed records。
- 排除 Pending 和 Deleted tombstone。
- 当前阶段默认按 `object_key` 字典序返回确定性列表。
- 分页、权限、多租户、版本化不属于当前阶段。

### Response

| Field | Description |
|-------|-------------|
| `records` | Committed MetadataRecord 列表 |
| `next_page_token` | 预留字段，可为空 |

## API Evolution Boundary

- Metadata API 是 KV API 的语义升级，不应破坏现有 RaftService。
- 如果后续新增 protobuf service，应新增 `MetadataService` 而不是修改 Raft 内核 RPC。
- 如果保留 KV demo 并新增 metadata demo，CLI 名称和输出格式应明确区分，避免脚本误用。
- Metadata API 不承诺真实数据上传、下载、chunk 存储或 StorageNode 可达性。
