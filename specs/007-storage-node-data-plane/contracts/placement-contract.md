# Contract Draft: PlacementManager And ReplicaPolicy

**Feature**: `007-storage-node-data-plane`  
**Status**: Draft planning contract; no production API or proto is changed in this phase.

## Purpose

PlacementManager selects StorageNode replicas for each chunk. ReplicaPolicy defines how many replicas are required, how many writes must succeed before metadata commit, how reads choose replicas, and when a chunk is under-replicated.

## Hard Boundaries

- Placement never writes object payload to Raft.
- Placement never marks object committed.
- Metadata commit happens only after selected StorageNode writes satisfy the policy.
- `ChunkRef.replica_nodes` remains the MVP metadata carrier for committed replica locations.
- StorageNode heartbeat is independent of Raft heartbeat.
- MVP does not use erasure coding.

## Inputs

| Input | Source | Purpose |
|-------|--------|---------|
| `chunk_id` | upload coordinator | Identify placement unit |
| `object_id` / `version` / `chunk_index` | metadata/coordinator | Build diagnosable chunk identity |
| `replica_policy` | configuration | replica count and write success rule |
| `storage_node_registry` | heartbeat/registry | health, capacity, load, disk pressure |
| `excluded_nodes` | caller/failure cache | avoid recently failed/corrupted targets |
| `failure_domain_hint` | registry placeholder | avoid same host/rack/zone in future |
| `hotspot_signals` | load/read stats | avoid overloaded hot nodes |

## PlacementDecision

| Field | Description |
|-------|-------------|
| `chunk_id` | chunk being placed |
| `replica_nodes` | ordered selected target nodes |
| `required_replica_count` | default 3 |
| `minimum_successful_writes` | default 2 |
| `excluded_nodes` | nodes skipped with reasons |
| `decision_epoch` | registry snapshot/version |
| `reasons` | capacity/health/load/disk/failure-domain notes |

## Default ReplicaPolicy

- `replica_count = 3`
- `minimum_successful_writes = 2`
- write succeeds only after at least 2 selected nodes return durable `WriteChunk` success for the same checksum/size.
- read selection prefers healthy, verified, lower-load replicas; it must skip corrupted, unavailable, overloaded, stale, or disk-pressure replicas where alternatives exist.
- under-replicated if healthy durable replicas are below policy target; critical if below minimum read safety threshold.
- erasure coding disabled.

## Node Eligibility

Placement should exclude or strongly down-rank nodes that are:

- not registered or heartbeat stale;
- `UNAVAILABLE`, `DRAINING`, or `READ_ONLY` for writes;
- disk full or high disk pressure;
- insufficient available capacity for chunk size plus reserve;
- overloaded beyond admission threshold;
- recently failed for the same chunk/object;
- known to host a corrupted replica of the same chunk;
- in same failure domain when enough alternatives exist.

## Write Success Semantics

1. Coordinator calls Placement for each chunk.
2. Coordinator sends `WriteChunk` to selected `replica_nodes`.
3. Each StorageNode returns `durable = true` only after staging, checksum verification, flush/publish, and parent directory durability boundary per platform contract.
4. If at least `minimum_successful_writes` succeed with matching checksum/size, the chunk is eligible for manifest commit.
5. `CommitObject` writes only successful durable replica nodes into `ChunkRef.replica_nodes`.
6. Failed or timed-out writes become cleanup candidates and cannot make object visible.

## Read Replica Selection

Inputs:

- `ChunkRef.replica_nodes`
- expected `ChunkRef.checksum` and `size`
- registry health/load facts
- known corruption/failure cache
- caller locality/failure preference if later added

Rules:

- Never read an object before metadata says COMMITTED.
- Sort candidates by health, recent success, load, locality placeholder, and lower failure count.
- On timeout/node unavailable, try next candidate within request deadline.
- On checksum mismatch, mark replica corrupted and do not return its data.
- If no healthy verified replica remains, return explicit read failure and trigger repair signal.

## Failed Replica Handling

- Record per-replica failure with reason: timeout, node unavailable, checksum mismatch, corrupted, IO error.
- Short-term failure facts influence Placement and read fallback.
- Persistent missing/corrupted facts create RepairTask candidates.
- Failed write replicas from uncommitted uploads become GC candidates after AbortObject or pending timeout.

## Under-Replicated Detection

A chunk is under-replicated when:

- committed manifest references fewer healthy replicas than `replica_count`;
- referenced replicas exist but heartbeat/liveness marks too many unavailable;
- read/scrub detects checksum mismatch or corruption;
- local `StatChunk` reports missing/deleted for a manifest replica.

Under-replicated detection must not directly mutate object state. It emits RepairTask candidates.

## Idempotency

- Placement evaluation is deterministic for a fixed registry snapshot and policy, but callers must tolerate a new decision after failures or timeout.
- Retrying the same upload request may reuse prior successful durable replicas.
- `WriteChunk` idempotency is owned by StorageNode and must reject mismatched content for same `chunk_id`.

## Timeout And Cancellation

- Placement calls have short bounded deadlines.
- Upload coordinator owns the global upload deadline.
- Cancellation stops issuing new `WriteChunk` calls and triggers AbortObject or pending cleanup policy.
- Durable writes completed before cancellation must be either committed in metadata or later collected by metadata-driven GC.

## Observability

Placement should expose:

- selected nodes and reasons;
- excluded nodes and reasons;
- policy parameters used;
- write success/failure counts;
- under-replicated detection counts;
- capacity/health/load snapshots used for decision.
