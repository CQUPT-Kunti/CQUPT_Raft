# Contract: Metadata Client CLI After KV Removal

## Goal

`raft_metadata_client` 成为唯一业务 CLI。它负责验证 metadata-only 主路径，而不是传输真实对象数据。

## Removed CLI Surface

- `raft_kv_client put`
- `raft_kv_client get`
- `raft_kv_client delete`
- `raft_kv_client status`
- `raft_kv_client health`
- `raft_kv_client metrics`

## Planned CLI Surface

```bash
raft_metadata_client <addr> create-bucket \
  --request-id req-bucket-1 \
  --bucket bucket-a

raft_metadata_client <addr> delete-bucket \
  --request-id req-bucket-del-1 \
  --bucket bucket-a

raft_metadata_client <addr> create-object \
  --request-id req-create-1 \
  --bucket bucket-a \
  --object object/a \
  --object-size 1048576 \
  --chunk-size 262144 \
  --chunk-count 4 \
  --checksum checksum-a \
  --mock-location node-a/chunk-0 \
  --mock-location node-b/chunk-1

raft_metadata_client <addr> commit-object \
  --request-id req-commit-1 \
  --bucket bucket-a \
  --object object/a \
  --expected-create-request-id req-create-1

raft_metadata_client <addr> abort-object \
  --request-id req-abort-1 \
  --bucket bucket-a \
  --object object/a

raft_metadata_client <addr> delete-object \
  --request-id req-delete-1 \
  --bucket bucket-a \
  --object object/a

raft_metadata_client <addr> head-object \
  --bucket bucket-a \
  --object object/a

raft_metadata_client <addr> list-objects \
  --bucket bucket-a \
  [--prefix object/] [--limit 100] [--page-token token]
```

## Retry / Failover Rules

- `NOT_LEADER` 和 `TIMEOUT` 重试必须复用同一 `request_id`
- 自动重试只允许改变目标地址，不允许改变请求身份
- `OVERLOADED` 默认不自动重试，除非用户显式开启有限重试
- `IDEMPOTENCY_CONFLICT` / `STATE_CONFLICT` 不自动重试

## Output Contract

CLI 输出必须稳定包含：

- `code`
- `message`
- `request_id`
- `bucket_name`
- `object_key`
- `state`
- `term`
- `log_index`
- `leader_id`
- `leader_address`

如果发生 retry，还必须输出：

- `attempt`
- `max_retries`
- `redirect_target`
- `final_target`

## Non-Goals

- 不读取真实文件
- 不上传 chunk bytes
- 不生成真实对象内容
- 不校验 DataNode 是否存在
- 不把 client 变成 storage uploader
