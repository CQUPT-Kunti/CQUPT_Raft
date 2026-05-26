# Cross-Task Risk Notes: Storage Node Data Plane

**Created**: 2026-05-25  
**Updated**: 2026-05-25  
**Related Feature**: [spec.md](./spec.md), [plan.md](./plan.md)

## Risks Against 006 No-KV Closure

- **Mis-restoring KV**: Future StorageNode work must not reintroduce `CommandType::kSet`, `CommandType::kDelete`, `KvStateMachine`, `KvService`, `raft_kv_client`, `DebugGetValue`, KV proto, KV target, KV fallback, or KV-only regression paths.
- **KV-style tests**: tests must not use `SetCommand`, `DeleteCommand`, `DebugGetValue`, KV state machine assertions, or regression-only KV paths as shortcuts for StorageNode validation.
- **StorageNode-as-KV risk**: Chunk storage must be modeled as object chunk data-plane storage, not as a key/value store that revives old demo semantics.
- **Fallback creep**: Do not add KV compatibility or fallback "temporarily" to make old tests pass. Any old no-KV audit failures should be fixed by metadata/data-plane paths.

## Control-Plane / Data-Plane Risks

- **Object payload in Raft**: Upload implementation must not put object bytes into Raft log, metadata snapshot, or Raft snapshot publish paths. Raft continues to replicate metadata intent and manifest only.
- **Metadata ownership risk**: StorageNode must not decide object committed/deleted visibility. Metadata remains the source of truth for object state and manifest.
- **Commit ordering risk**: `CommitObject` must only occur after each chunk satisfies minimum durable replica success. Committing before data durability would expose unreadable objects.
- **Read ordering risk**: Read path must query metadata first and reject uncommitted/deleted objects before contacting StorageNode.
- **Delete ordering risk**: Delete path must commit metadata tombstone/DELETED first; physical chunk deletion is background async cleanup.

## Durability And Recovery Risks

- **Raft durability substitution risk**: Existing Raft storage durability cannot be treated as chunk durability. StorageNode needs independent staging, checksum, fsync/FlushFileBuffers, atomic publish, parent directory sync, restart recovery, ChunkIndex rebuild, stale staging cleanup, partial write detection, and quarantine semantics.
- **Windows durability hidden by Linux assumptions**: Linux-only `rename` + `fsync` assumptions may hide Windows risks around file handles, `FlushFileBuffers`, `MoveFileEx` / `ReplaceFile`, long path, UTF-8 path, sharing mode, permission errors, and disk full.
- **No-op success risk**: Any required durability operation that cannot be implemented on a platform must return explicit error or be documented as a weaker contract. Silent no-op success is prohibited.
- **Crash window risk**: Crash before fsync, after fsync before rename, and after rename before parent directory sync must be tested separately.
- **Index rebuild risk**: Restart scanning must never promote partial staging or corrupted files into live ChunkIndex.

## GC / Repair / Rebalance Risks

- **GC live-delete risk**: GarbageCollector must be metadata-driven and must not delete any chunk referenced by a committed live manifest, even if local scans classify it as orphan or suspicious.
- **Pending cleanup risk**: failed upload, client disconnect, pending timeout, and AbortObject cleanup must not commit partial objects and must not leak durable orphan chunks indefinitely.
- **Repair manifest risk**: RepairManager must copy from a verified healthy source to a healthy target and make the new replica durable before metadata update. Corrupted replicas must never be repair sources.
- **Rebalance half-migration risk**: RebalanceManager must avoid half-migrated manifests. Target durable before manifest update; source cleanup only after metadata no longer requires source.
- **Concurrent background task risk**: GC, Repair, Scrub, and Rebalance need per-chunk coordination so one task does not delete a source needed by another.

## Proto / Module Boundary Risks

- **Proto extension conflict**: Future StorageNode proto work may conflict with existing `metadata.proto` / `common.proto` boundaries. `MetadataService` should remain bucket/object lifecycle; StorageNode chunk IO should use an independent service contract unless a later plan explicitly justifies shared messages.
- **ChunkRef overextension risk**: Existing `ChunkRef` is sufficient for MVP manifest, but per-replica health, corruption, last_verified_at, and local path should not be forced into it without a metadata/proto compatibility plan.
- **Raft storage module confusion**: `modules/raft/storage` owns Raft hard state, segment log, and snapshot catalog. StorageNode code should use a distinct module boundary to avoid persisted format and durability contract confusion.
- **Public API drift**: Any real proto or public API change must be a later explicit task with caller, CMake, and tests synchronized.

## Concurrency And Test Isolation Risks

- **Unbounded concurrency risk**: StorageNode must use bounded thread pool, executor, request queue, IO queue, rate limit, timeout, cancellation, and resource isolation. Unbounded parallel writes can exhaust memory or disk.
- **Lock contention risk**: `ChunkIndex` needs sharding/lock striping/read-write locks and per-chunk locks; a single global lock may hide correctness but fail high-concurrency goals.
- **Test interference risk**: Storage high-concurrency tests may run in parallel, but recovery/snapshot/catch-up/crash-boundary tests should remain low concurrency and may require `CTEST_PARALLEL_LEVEL=1`.
- **Artifact pollution risk**: Test-generated `raft_data/`, `raft_snapshots/`, chunk dirs, logs, and build artifacts must not be treated as source analysis inputs.

## Current Baseline Confirmation

- `RaftNode` default assembly points to `MetadataStateMachine`.
- `RaftNode` registers `RaftService` and `MetadataService`; no `KvService` registration was found in the current narrow review.
- `MetadataService` and `MetadataStateMachine` cover bucket/object lifecycle and committed-only query semantics.
- `ObjectRecord.chunks` / `ChunkRef` / `chunk_ref_index_` provide a metadata manifest base but do not imply real chunk IO.
- Narrow search did not find real `StorageNode`, `PlacementManager`, `RepairManager`, `RebalanceManager`, `ScrubManager`, `WriteChunk`, `ReadChunk`, or `DeleteChunk` implementation in production modules.
- no-KV hits in the narrow review are audit-script literals and metadata delete names, not evidence of restored old KV production path.

## Follow-Up For Tasks Phase

- Split implementation so LocalDiskChunkStore durability and restart recovery land before network, Placement, Repair, or Rebalance.
- Add no-KV audit as a recurring validation dependency for all StorageNode tasks.
- Define exact future module path and target names without modifying current CMake in plan phase.
- Decide whether StorageNode service uses a new proto file or staged draft-to-real migration.
- Define Windows/Linux validation filters before writing platform durability code.
