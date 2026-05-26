# Contract Draft: StorageNode API

**Feature**: `007-storage-node-data-plane`  
**Status**: Draft only. Do not treat this as real proto or implemented API.  
**Boundary**: StorageNode APIs move chunk bytes between clients/coordinators/StorageNodes and local chunk store. They do not decide object committed/deleted visibility.

## Shared Rules

- All requests carry `request_id`, `node_id` target where applicable, and a deadline/timeout from caller.
- All responses carry `status`, `error_code`, `message`, `retry_after_ms` when overloaded or retryable, and enough identity fields for idempotent retry.
- Error categories: `OK`, `ALREADY_EXISTS`, `NOT_FOUND`, `CONFLICT`, `CHECKSUM_MISMATCH`, `CORRUPTED`, `DISK_FULL`, `PERMISSION_DENIED`, `IO_ERROR`, `TIMEOUT`, `CANCELLED`, `OVERLOADED`, `NODE_UNAVAILABLE`, `UNSUPPORTED`, `INVALID_ARGUMENT`.
- Retry rules: retry `TIMEOUT`, `OVERLOADED`, `NODE_UNAVAILABLE`, and selected `IO_ERROR` only within caller retry budget; do not retry `CONFLICT`, `PERMISSION_DENIED`, `INVALID_ARGUMENT`, or checksum mismatch without a new decision.
- Cancellation: cancelled calls must stop admission when possible; in-flight writes that already published a chunk must return the durable result on retry via idempotency.
- Checksum: write/read/scrub/repair/migration must verify checksum. Checksum mismatch never returns success.
- Concurrency: per-chunk lock serializes conflicting write/delete/repair/rebalance operations; reads may run concurrently with other reads and must not observe staging files as live chunks.
- Object payload bytes never go through Raft log or metadata snapshot.

## WriteChunk

**Request Fields**

| Field | Description |
|-------|-------------|
| `request_id` | idempotency key |
| `chunk_id` | MVP `object_id + version + chunk_index` |
| `object_id` / `version` / `chunk_index` | diagnostic identity |
| `offset` | object offset |
| `expected_size` | expected chunk size |
| `expected_checksum` | expected checksum |
| `payload` or stream | chunk bytes |
| `durability` | required durable publish policy |

**Response Fields**

| Field | Description |
|-------|-------------|
| `chunk_id` | written chunk |
| `node_id` | StorageNode that accepted data |
| `size` | durable size |
| `checksum` | computed checksum |
| `state` | `LIVE` on success |
| `durable` | true only after required flush/publish boundary |
| `already_exists` | true for idempotent duplicate |
| `status` / `error_code` | result |

**Idempotency**: same `chunk_id` + same size/checksum/content identity returns success or `already_exists`; mismatch returns `CONFLICT` or `CHECKSUM_MISMATCH`.  
**Retry**: safe to retry after timeout if same request identity and payload identity are used.  
**Timeout/Cancellation**: cancellation before publish can leave stale staging; restart cleanup handles it. Cancellation after publish returns durable result on retry.  
**Checksum**: compute during write, compare with expected before publish, persist checksum in local metadata/index.  
**Concurrency**: exclusive per-chunk write lock; concurrent read of same chunk only allowed after final publish.

## ReadChunk

**Request Fields**: `request_id`, `chunk_id`, optional `range_offset`, optional `range_size`, `expected_checksum`, `verify_checksum`, caller deadline.  
**Response Fields**: `chunk_id`, `node_id`, `size`, `checksum`, `payload` or stream, `verified`, `status`, `error_code`.

**Idempotency**: read has no side effect except optional health/corruption observation.  
**Retry**: retry on timeout/node unavailable; on checksum mismatch mark replica corrupted and try another replica.  
**Timeout/Cancellation**: stream can be cancelled; partial response must not be treated as valid chunk.  
**Checksum**: full chunk reads verify full checksum; range reads either verify full chunk internally or return range with a documented verification limitation. MVP should prefer full verification.  
**Concurrency**: multiple reads can share read lock; read must not serve `STAGING`, `DELETING`, `CORRUPTED`, or `QUARANTINED`.

## DeleteChunk

**Request Fields**: `request_id`, `chunk_id`, `reason`, `metadata_boundary`, `delay_until`, optional `expected_checksum`.  
**Response Fields**: `chunk_id`, `node_id`, `deleted`, `already_missing`, `tombstoned`, `status`, `error_code`.

**Idempotency**: repeated delete for the same chunk is success if chunk is already missing/deleted/tombstoned.  
**Retry**: retry timeout/IO error according to GC retry budget.  
**Timeout/Cancellation**: cancellation may leave `DELETING`; next retry continues.  
**Checksum**: optional expected checksum prevents deleting a different conflicting local file.  
**Concurrency**: delete takes per-chunk write lock; live reads already admitted may finish or be cancelled by policy, new reads after delete/tombstone fail.

## StatChunk

**Request Fields**: `request_id`, `chunk_id`, optional `include_quarantine`, optional `verify_checksum`.  
**Response Fields**: `chunk_id`, `node_id`, `state`, `size`, `checksum`, `last_verified_at`, `last_error`, `status`, `error_code`.

**Idempotency**: read-only.  
**Retry**: retry node unavailable/timeout only.  
**Checksum**: if `verify_checksum` true, may promote mismatch to `CORRUPTED`.  
**Concurrency**: read lock on ChunkIndex; optional file verify must not block unrelated shards.

## ListChunks

**Request Fields**: `request_id`, `page_token`, `page_size`, `state_filter`, `prefix_filter`, `include_quarantine`.  
**Response Fields**: `chunks[]`, `next_page_token`, `snapshot_epoch`, `status`, `error_code`.

**Idempotency**: read-only paginated scan.  
**Retry**: caller may restart from last page token; page tokens are best-effort within index snapshot epoch.  
**Checksum**: no full checksum verification by default; use `ScrubChunk` for validation.  
**Concurrency**: shard-by-shard scan, bounded page size.

## BatchDeleteChunks

**Request Fields**: `request_id`, `items[] {chunk_id, expected_checksum, reason, metadata_boundary}`, `max_parallelism`, `delay_until`.  
**Response Fields**: per-item result, aggregate counts, `status`, `error_code`.

**Idempotency**: each item follows `DeleteChunk` semantics.  
**Retry**: retry failed retryable items only.  
**Timeout/Cancellation**: partial batch results must identify completed items.  
**Checksum**: optional per-item expected checksum.  
**Concurrency**: bounded internal parallelism; backpressure if delete queue is full.

## ScrubChunk

**Request Fields**: `request_id`, `chunk_id`, `expected_checksum`, `repair_hint`, `priority`.  
**Response Fields**: `chunk_id`, `verified`, `state`, `checksum`, `corruption_reason`, `status`, `error_code`.

**Idempotency**: repeated scrub is safe; may refresh `last_verified_at`.  
**Retry**: retry IO timeout only; checksum mismatch is terminal for that replica.  
**Timeout/Cancellation**: cancelled scrub leaves existing state unless corruption already confirmed.  
**Checksum**: full verification required.  
**Concurrency**: read lock unless quarantine transition is needed.

## RepairChunk

**Request Fields**: `request_id`, `chunk_id`, `source_node`, `target_node`, `expected_size`, `expected_checksum`, `repair_task_id`.  
**Response Fields**: `chunk_id`, `target_node`, `size`, `checksum`, `durable`, `already_exists`, `status`, `error_code`.

**Idempotency**: same task to same target with matching checksum is success/already_exists; conflicting content fails.  
**Retry**: retry after source/target timeout if source replica remains healthy.  
**Timeout/Cancellation**: target may leave staging; cleanup/retry handles it.  
**Checksum**: verify source read and target write; target durable before success.  
**Concurrency**: source read lock, target write lock; must not race with GC deleting live source.

## ReportHealth

**Request Fields**: `request_id`, `node_id`, `health`, `disk_pressure`, `io_error_count`, `corruption_count`, `last_error`.  
**Response Fields**: `accepted`, `registry_epoch`, `status`, `error_code`.

**Idempotency**: latest sequence wins; duplicate heartbeat accepted.  
**Retry**: retry control-plane unavailable/timeout.  
**Timeout/Cancellation**: no partial side effects beyond accepted registry update.  
**Checksum**: not applicable.  
**Concurrency**: registry update must be atomic per node.

## ReportCapacity

**Request Fields**: `request_id`, `node_id`, `capacity`, `used`, `available`, `chunk_count`, `reserved`.  
**Response Fields**: `accepted`, `registry_epoch`, `status`, `error_code`.

**Idempotency**: latest sequence wins.  
**Retry**: safe with same sequence.  
**Concurrency**: per-node registry update.

## ReportLoad

**Request Fields**: `request_id`, `node_id`, `active_reads`, `active_writes`, `queued_ops`, `io_latency`, `bandwidth`, `hot_chunks`.  
**Response Fields**: `accepted`, `registry_epoch`, `status`, `error_code`.

**Idempotency**: latest sequence wins; stale sequence ignored.  
**Retry**: safe.  
**Concurrency**: per-node registry update.

## RegisterStorageNode

**Request Fields**: `request_id`, `node_id`, `endpoint`, `data_dir_fingerprint`, `failure_domain`, `capabilities`.  
**Response Fields**: `registered`, `already_registered`, `registry_epoch`, `status`, `error_code`.

**Idempotency**: same `node_id` + endpoint/capabilities is success; different endpoint requires explicit re-registration flow or conflict.  
**Retry**: safe with same identity.  
**Concurrency**: registry must serialize updates for same node.

## UpdateStorageNodeHeartbeat

**Request Fields**: combined health/capacity/load fields plus `sequence`, `sent_at`.  
**Response Fields**: `accepted`, `stale`, `registry_epoch`, `status`, `error_code`, optional control hints such as `drain` or `rate_limit`.

**Idempotency**: duplicate sequence accepted or marked stale without side effect.  
**Retry**: safe with same sequence.  
**Concurrency**: per-node atomic replace of heartbeat facts.
