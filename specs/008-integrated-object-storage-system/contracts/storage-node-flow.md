# Contract: StorageNode Data Flow

**Purpose**: 定义 StorageNode 保存真实 chunk 数据的行为边界。

## Responsibilities

- 接收 bounded chunk 写入。
- 校验 expected checksum 和 size。
- 执行 staging -> durable flush -> publish。
- 返回 chunk_id、node_id、size、checksum、state、durable。
- 按 manifest 读取 chunk 并可执行 checksum 校验。
- 删除未提交或不再需要的 chunk。
- 重启后恢复 published chunk catalog，并识别 incomplete staging 数据。

## WriteChunk

**Input**:

- `request_id`
- `chunk_id`
- `object_id`
- `version`
- `chunk_index`
- `offset`
- `expected_size`
- `expected_checksum`
- bounded `payload`
- durability mode

**Output**:

- status
- node_id
- chunk_id
- size
- checksum
- state
- durable
- already_exists

**Rules**:

- Payload size must not exceed configured chunk size.
- Checksum mismatch returns integrity failure and does not publish live chunk.
- Durable success requires platform-specific flush/publish contract.
- Idempotent retry of same chunk/checksum may return already_exists.

## ReadChunk

**Input**:

- `request_id`
- `chunk_id`
- object identity
- offset/length
- expected checksum
- verify checksum flag

**Output**:

- status
- payload bytes for requested range
- size
- checksum
- complete/full_read flags

**Rules**:

- If expected checksum is present and verification fails, return checksum mismatch.
- Read path must not consult ViewNode for object visibility; visibility comes from MetadataNode manifest.

## DeleteChunk / BatchDeleteChunks

**Input**:

- chunk identity
- expected checksum
- reason
- metadata boundary

**Output**:

- per-chunk status
- retryable flag
- idempotency flags

**Rules**:

- Cleanup must be safe for missing/already deleted chunks.
- Cleanup caller must prove metadata boundary; StorageNode does not decide object visibility.

## Recovery

- LIVE published chunks must remain readable after restart.
- STAGING/incomplete chunks must not be reported as committed data.
- Corrupted chunks must be quarantined or reported as corrupted; no silent success.

## Platform Durability

- Linux: file data and directory publish operations require real durability calls where contract marks them required.
- Windows: equivalent file handle flush/publish behavior must be used, or the operation must return explicit unsupported/weak-contract status.
- No platform branch may return success for required durability as a no-op.
