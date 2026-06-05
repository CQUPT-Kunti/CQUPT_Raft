# Contract Draft: GC, Scrub, Repair, Rebalance

**Feature**: `007-storage-node-data-plane`  
**Status**: Draft planning contract; no production API or tests are changed in this phase.

## Shared Boundaries

- Metadata is the source of truth for object state and live chunk manifest.
- GC, Repair, Scrub, and Rebalance never store object payload in Raft.
- StorageNode does not decide whether an object is committed or deleted.
- New replica or migrated target must be durable before metadata manifest coordination.
- Committed live manifest protection overrides local orphan scan conclusions.
- All background tasks are bounded, retryable, idempotent, observable, and cancellable.

## GarbageCollector Contract

### Inputs

- metadata tombstone/DELETED facts;
- `AbortObject` facts;
- pending object timeout;
- client disconnect cleanup signal;
- failed upload write results;
- StorageNode `ListChunks` orphan candidates;
- committed live manifest snapshot for protection.

### Task Fields

| Field | Description |
|-------|-------------|
| `task_id` | idempotent GC identity |
| `chunk_id` | target chunk |
| `target_nodes` | nodes to delete from |
| `reason` | pending_timeout / abort / failed_upload / object_deleted / orphan |
| `metadata_boundary` | applied index/view/tombstone fact used for safety |
| `state` | pending/running/retry_wait/succeeded/failed |
| `attempts` / `last_error` | retry diagnostics |

### Safety Rules

- Delete only after metadata says the object is not committed live or the chunk is not referenced by any committed live manifest.
- Local orphan scan may propose candidates but cannot authorize deletion alone.
- `DeleteObject` visibility is immediate after metadata tombstone/DELETED; physical chunk deletion is async.
- `DeleteChunk` and `BatchDeleteChunks` are idempotent; missing/deleted is success.
- Delayed physical deletion and chunk tombstone are allowed.
- GC must not delete source replicas currently required by Repair/Rebalance unless metadata/coordination proves another healthy durable replica remains.

### Retry, Timeout, Cancellation

- Retry retryable `TIMEOUT`, `OVERLOADED`, `NODE_UNAVAILABLE`, and selected `IO_ERROR`.
- Stop or pause when queue is over bounded limits.
- Cancellation leaves task retryable unless metadata state changes make it obsolete.

## ScrubManager Contract

### Inputs

- scheduled background scan;
- read-path checksum mismatch signal;
- repair verification request;
- operator-triggered scrub request.

### Rules

- `ScrubChunk` performs full checksum validation against expected checksum.
- On mismatch, mark replica corrupted/quarantined and emit RepairTask candidate.
- Corrupted replica cannot be used as read source or repair source.
- Scrub uses bounded low-priority IO queue and must yield to foreground reads/writes.
- Scrub progress and corruption findings are observable.

## RepairManager Contract

### Inputs

- under-replicated detection;
- missing replica facts;
- corrupted replica facts;
- StorageNode heartbeat/liveness changes;
- ScrubManager findings;
- read-path checksum mismatch.

### Task Fields

| Field | Description |
|-------|-------------|
| `task_id` | idempotent repair identity |
| `chunk_id` | chunk to repair |
| `source_node` | verified healthy source |
| `target_node` | selected healthy target |
| `expected_checksum` / `size` | manifest expected facts |
| `reason` | missing / corrupted / under_replicated |
| `state` | pending/copying/verifying/target_durable/updating_metadata/succeeded/failed |
| `progress` | bytes copied and percent |
| `attempts` / `last_error` | retry diagnostics |

### Rules

- Choose a healthy source replica whose checksum matches metadata manifest.
- Choose target via Placement eligibility: capacity, health, load, disk pressure, failure-domain placeholder.
- Copy chunk between StorageNodes; do not route bytes through Raft.
- Verify checksum on source read and target write.
- Target replica must be durable before metadata manifest update or replica health update.
- Repair task retry is idempotent; already existing target with matching checksum is success.
- Do not update manifest to remove the last healthy source until replacement is durable and visible.

### Retry, Timeout, Cancellation

- Retry source/target timeout and node unavailable within bounded attempts.
- Cancel repair if metadata no longer references chunk or object is deleted.
- Backoff on repeated IO errors and mark node degraded when appropriate.

## RebalanceManager Contract

### Inputs

- capacity imbalance;
- hotspot signals;
- new node join;
- draining node;
- maintenance operation;
- Placement policy and registry facts.

### Task Fields

| Field | Description |
|-------|-------------|
| `task_id` | idempotent rebalance identity |
| `chunk_id` | migrating chunk |
| `source_node` | existing healthy source |
| `target_node` | selected target |
| `reason` | capacity / hotspot / new_node_join / drain |
| `expected_checksum` / `size` | manifest expected facts |
| `state` | pending/copying/verifying/target_durable/updating_manifest/source_cleanup/succeeded/failed |
| `progress` | bytes copied and percent |
| `attempts` / `last_error` | retry diagnostics |

### Rules

- Migration target must be durable before manifest coordination.
- Avoid half-migrated manifest: readers must always have at least one healthy manifest replica.
- Source cleanup happens only after metadata no longer requires that source replica.
- Rebalance must coordinate with GC and Repair per chunk to avoid deleting an active source or duplicating work.
- Rebalance is background and lower priority than foreground reads/writes and urgent repair.
- New node join can gradually receive chunks; no all-at-once migration.

### Retry, Timeout, Cancellation

- Retry copy/verify failure when source remains healthy and target remains eligible.
- Cancel if object is deleted, chunk no longer in committed manifest, or target becomes ineligible before publish.
- Idempotent target write follows `WriteChunk` rules.

## Manifest Coordination Requirements

Future implementation must define how Repair/Rebalance updates manifest facts. Until a dedicated metadata update contract exists:

- do not mutate `ObjectRecord.chunks` outside Raft metadata commit path;
- do not let StorageNode write metadata directly;
- do not remove existing healthy replicas from manifest before replacement is durable;
- represent per-replica corruption/health outside current `ChunkRef` unless proto/metadata extension is explicitly planned;
- preserve request_id idempotency for any metadata update command added later.

## Observability

Each manager should expose:

- queue depth and bounded capacity;
- active/running task count;
- success/failure/retry counts;
- last error by category;
- bytes copied/deleted/scrubbed;
- per-task progress;
- corrupted/missing/under-replicated counts;
- rate limiting and backpressure events.
