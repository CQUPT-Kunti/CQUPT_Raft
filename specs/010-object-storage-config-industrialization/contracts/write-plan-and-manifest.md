# Contract: Write Plan And Manifest

## Scope

This contract defines the first-stage behavior for CreateWritePlan, upload execution, and committed manifest facts.

## CreateWritePlan Output

CreateWritePlan must return a plan that can express:

- `object_id`
- `upload_session_id` or request/session identity
- `chunk_size_bytes`
- `total_chunks`
- `replica_count`
- `minimum_successful_writes`
- `placement_epoch`
- `plan_expire_at`
- per-chunk identity, offset, expected size, expected checksum
- per-chunk selected replica nodes
- optional per-chunk candidate nodes or diagnostic exclusions

The first-stage implementation should reuse `TransferWritePlan` and `TransferChunkPlan`. It should not introduce a parallel family of Plan/Context/Result structs unless existing types cannot carry the required facts.

## Placement Authority

- Each chunk is the placement unit.
- Store nodes are not Raft groups.
- Different chunks may have completely different replica sets.
- `node_id` is identity only. It may appear in selected nodes, RPC routing, manifest and diagnostics; it must not be lexical priority.
- If the plan lacks selected replica nodes for a non-empty chunk, upload must fail before writing payload.

## Upload Consumption

- Upload must execute selected replica nodes from the plan.
- Upload must not fill missing replicas by sorting discovered StorageNodes by `node_id`, endpoint, name or configuration order.
- If a selected node is not currently discoverable, upload returns a plan/discovery error.
- CommitObject receives actual durable replicas only, not planned-but-failed replicas.

## Manifest

Committed manifest remains metadata-authoritative and payload-free.

For each committed chunk:

- `chunk_id` must match object/version/chunk index identity.
- `offset` and `size` must form a valid layout.
- `replica_nodes` must be the actual durable success nodes.
- `checksum` must be the expected verified checksum.

Pending, failed, aborted or deleted objects must not become visible through StorageNode discovery or local chunk presence.
