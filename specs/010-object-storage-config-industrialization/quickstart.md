# Quickstart: Object Storage Config Industrialization

## Build

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

Conservative fallback:

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## Targeted Tests

Use targeted tests first, then widen scope:

```bash
ctest --test-dir build/debug-ninja-low-parallel --output-on-failure -R "store_placement|storage_upload|storage_read|metadata_manifest|integrated_object_storage_concurrency"
```

Project test entry:

```bash
./test.sh --group unit
```

Full low-concurrency validation when ready:

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

## Expected First-Stage Evidence

- Placement test shows no `node_id` / config-order priority.
- 9-node placement test shows multi-chunk distribution across most healthy nodes.
- Upload test shows CreateWritePlan selected replica nodes are consumed without fallback.
- Fan-out test observes overlapping same-chunk replica writes.
- Multi-chunk test observes bounded `max_inflight_chunks` and `max_inflight_bytes`.
- Manifest test shows committed chunk refs preserve actual durable replica nodes and no payload.
- Read test shows same-chunk manifest replica fallback and checksum validation.

## Test Log Rule

Do not paste full logs into chat. Save logs under `tmp/test-logs/`. On success report command, PASS and duration only. On failure report failing test, key assertion, failure class, last 50 lines and full log path.
