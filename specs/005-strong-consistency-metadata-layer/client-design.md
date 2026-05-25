# Client Design: Metadata Client

## Purpose

Metadata Client 是 `raft_kv_client` 的规划后继，用于验证强一致元数据层，而不是传输真实文件。它负责构造 metadata API 请求、生成模拟对象日志、执行重试场景、验证 committed-only visibility，并输出可诊断结果。

## Responsibilities

- 发起 `CreateMetadataRecord`、`CommitMetadataRecord`、`DeleteMetadataRecord`、`HeadMetadataRecord`、`ListMetadataRecords`。
- 支持用户显式指定 `request_id`，用于模拟重复请求、超时重试、leader failover 后重试。
- 生成模拟 `object_key`、`object_size`、`chunk_size`、`chunk_count`、`checksum`、`mock_locations` 和 `payload`。
- 支持读后写验证：create 后验证不可见，commit 后验证可见，delete 后验证不可见。
- 输出 `code`、`message`、`leader_id`、`leader_address`、`term`、`log_index`、`request_id`、`state`。
- 根据 `NOT_LEADER` 响应提示用户或自动重定向到 leader，自动重试时必须复用同一 `request_id`。

## Non-Responsibilities

- 不读取真实文件内容。
- 不上传或下载大文件。
- 不写入真实 chunk 文件。
- 不检查 StorageNode 是否存在。
- 不执行 chunk replication、纠删码、rebalance 或 S3 协议。
- 不直接读取 Raft 日志、snapshot 文件或内部测试状态。

## Command Shape

完整 Metadata Client 仍是后续目标；当前阶段只有最小 failover retry 场景，不是完整 command dispatcher。

当前已经固定的最小 CLI 形态如下：

```bash
raft_metadata_client <addr> commit-retry \
  --request-id req-commit-1 \
  --object-key obj/a \
  --expected-create-request-id req-create-1 \
  --commit-info "commit after failover" \
  --max-retries 1 \
  --timeout-ms 3000

raft_metadata_client <addr> delete-retry \
  --request-id req-delete-1 \
  --object-key obj/a \
  --delete-info "delete after failover" \
  --max-retries 1 \
  --timeout-ms 3000
```

后续完整 Metadata Client 可采用类似以下 CLI 形态，具体命令名可在 tasks 阶段继续固定：

```bash
metadata_client <addr> create \
  --request-id req-001 \
  --object-key obj/a \
  --object-size 1048576 \
  --chunk-size 262144 \
  --chunk-count 4 \
  --checksum sha256:mock \
  --mock-location node1/chunk0 \
  --mock-location node2/chunk1 \
  --payload '{"demo":"metadata-only"}'

metadata_client <addr> commit \
  --request-id req-002 \
  --object-key obj/a \
  --expected-create-request-id req-001

metadata_client <addr> head --object-key obj/a

metadata_client <addr> list

metadata_client <addr> delete \
  --request-id req-003 \
  --object-key obj/a \
  --delete-info "demo delete"
```

当前限制：

- `raft_metadata_client` 目前只覆盖 `commit-retry` / `delete-retry`
- 当前不包含完整 `create` / `head` / `list` / `delete` dispatcher
- 当前尚未接入 CMake target，正式构建接入留到 `T033`

## Simulated Metadata Generation

### object_key

- 默认可由用户显式指定，例如 `demo/object-001`。
- 自动生成时可使用 `demo/<timestamp-or-sequence>`，但不能依赖系统时间提供一致性语义。
- 当前阶段不规划覆盖、版本化或 generation。

### object_size / chunk_size / chunk_count

- `object_size` 表示模拟对象大小。
- `chunk_size` 表示模拟切片大小，必须大于 0。
- `chunk_count` 可由用户指定，也可由客户端按 `ceil(object_size / chunk_size)` 生成。
- 客户端应允许故意传入不匹配值，以验证服务端 invalid argument 行为；默认模式应生成一致 manifest。

### checksum

- 默认可生成固定 mock checksum，例如 `sha256:<object_key>:<object_size>:<chunk_count>` 的摘要字符串。
- 当前阶段不读取文件、不计算真实文件 checksum。

### mock_locations

- 表示模拟 chunk 放置位置，例如 `node-1/chunk-0`、`node-2/chunk-1`。
- 不要求这些节点存在。
- 可按 chunk_count 自动生成一组位置，也可由用户多次传入。

### payload

- 只表示 metadata-only 附加信息，例如 JSON 字符串或普通文本。
- 必须有大小上限，防止真实文件内容进入 Raft。
- 客户端输出应明确标注 payload 是模拟 metadata，不是对象 bytes。

## Retry Scenarios

### Duplicate Create

流程：

1. 使用 `request_id=req-create-1` 创建 object。
2. 使用相同 `request_id` 和完全相同字段再次 create。
3. 期望返回 `IDEMPOTENT_REPLAY` 或等价成功，状态仍为 Pending，Head/List 不可见。

### Idempotency Conflict

流程：

1. 使用 `request_id=req-create-1` 创建 object。
2. 使用相同 `request_id` 但不同 `object_key`、checksum 或 manifest 再次 create。
3. 期望返回 `IDEMPOTENCY_CONFLICT`。

### Commit Retry

流程：

1. create 得到 Pending。
2. commit 使用 `request_id=req-commit-1`。
3. 如果 commit 返回 `NOT_LEADER` 或 `TIMEOUT`，客户端不得生成新的 request_id。
4. 若响应带有 `leader_hint.leader_address`，客户端应优先把它作为下一次 retry 目标地址；如果没有 leader hint，则提示用户对当前地址或已知 leader 地址继续重试。
5. retry 必须复用同一 `request_id=req-commit-1`，只允许改变目标地址，不允许改变 request identity。
6. retry 必须受 `max-retries` 限制，不能无限循环。
7. 期望最终只有一个 Committed 结果，Head/List 可见且无重复记录。

### Delete Retry

流程：

1. create + commit object。
2. delete 使用 `request_id=req-delete-1`。
3. 如果 delete 返回 `NOT_LEADER` 或 `TIMEOUT`，客户端不得生成新的 request_id。
4. 若响应带有 `leader_hint.leader_address`，客户端应优先把它作为下一次 retry 目标地址。
5. retry 必须复用同一 `request_id=req-delete-1`。
6. retry 必须受 `max-retries` 限制，不能无限循环。
7. 期望 tombstone 只表达一个删除事实，Head/List 不可见。

### Read-After-Write Verification

流程：

1. create 后立即 head/list，期望 not found。
2. commit 后 head/list，期望 found 且字段与 manifest 一致。
3. delete 后 head/list，期望 not found。
4. restart/failover 后重复 head/list，期望 committed 或 tombstone 结果稳定。

当前阶段建议按以下方式执行 Head/List 验证：

1. `commit-retry` 返回 `OK` 或 `IDEMPOTENT_REPLAY` 后，立即对 leader 执行 `HeadMetadataRecord`。
2. `HeadMetadataRecord` 期望返回 `found=true` 且 `state=COMMITTED`。
3. 随后执行 `ListMetadataRecords`，确认 `object_key` 出现在 committed-only 列表中。
4. `delete-retry` 返回 `OK` 或 `IDEMPOTENT_REPLAY` 后，再次执行 `HeadMetadataRecord`，期望 `found=false` 或 `NOT_FOUND`。
5. 随后执行 `ListMetadataRecords`，确认目标 `object_key` 不再出现。
6. 如果 `Head/List` 返回 `NOT_LEADER`，客户端应根据 leader hint 切换到 leader 后再继续验证，且这一步不涉及生成新的 write request_id。

## Output Format

建议输出保持单行 key/value 或稳定 JSON 二选一。无论格式如何，必须包含：

- `code`
- `message`
- `request_id`
- `object_key`
- `state`
- `leader_id`
- `leader_address`
- `term`
- `log_index`

对于 failover retry 场景，建议额外稳定输出：

- 当前请求投递地址
- retry 次数 / 下一次 retry 目标地址
- leader hint 是否来自本次响应

## Error Handling

- `NOT_LEADER`: 显示 `leader_id`、`leader_address`；如做自动 retry，必须复用同一 request_id。
- `TIMEOUT`: 不生成新 request_id；提示用户使用原 request_id 做有限次数重试。
- `INVALID_ARGUMENT`: 输出具体字段原因。
- `IDEMPOTENCY_CONFLICT`: 输出冲突 request_id 和冲突类别，不自动重试。
- `STATE_CONFLICT`: 输出当前状态和期望状态，不自动重试。

## Current Stage Notes

- 当前 `raft_metadata_client` 只是最小 failover retry 场景实现，不是完整 Metadata Client。
- 当前只覆盖 `commit-retry` / `delete-retry`。
- 当前不实现真实文件上传下载，不实现 chunk 生成，不实现完整 read-after-write 自动化流程。
- 当前尚未接入 CMake target，正式构建接入留到 `T033`。

## Future Boundary

未来接入 StorageNode/ChunkStore 时，Metadata Client 可以扩展为上传协调器，但本阶段不做：

- 不打开本地文件。
- 不切分真实文件。
- 不向 StorageNode 传输 bytes。
- 不校验真实 chunk 落盘。
- 不处理 chunk replica placement。

当前 Client 只模拟 manifest 和 location 引用，验证 metadata control plane。
