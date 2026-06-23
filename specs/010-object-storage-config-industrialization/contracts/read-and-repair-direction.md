# Contract: Read And Repair Direction

## Manifest-Driven Read

Download must use committed metadata manifest as the object visibility authority.

For each chunk:

- Read candidates come from that chunk's manifest `replica_nodes`.
- Observed health/load facts may reorder or exclude candidates.
- Missing observed facts may be treated as neutral fallback, but only for nodes already in manifest.
- A failed replica read should be followed by another same-chunk manifest replica when available.
- A successful read must pass size and checksum validation before bytes are appended to output.

## Failure Handling

If all replicas for a chunk fail:

- Return a chunk-scoped read error.
- Include attempted node ids, retryability and checksum/missing/corruption facts where available.
- Do not read from non-manifest nodes as a hidden repair or discovery fallback.

## Repair Decision B

When a replica is missing or corrupted, future repair should:

- keep healthy existing replicas as sources,
- call placement with current resource facts,
- exclude unsuitable or already-hosting nodes,
- select a new target that may differ from the original missing node,
- write and verify the new replica,
- update metadata manifest only after durable verification.

Stage 1 does not implement full manifest coordination for repair. It only ensures placement and manifest facts are compatible with this future flow.
