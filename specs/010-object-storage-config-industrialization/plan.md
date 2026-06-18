# Implementation Plan: Object Storage Config Industrialization

**Branch**: `010-object-storage-config-industrialization` | **Date**: 2026-06-18 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/010-object-storage-config-industrialization/spec.md`

## Summary

010 阶段把 store 上传路径从“串行且能跑”推进到“per-chunk dynamic placement + bounded high-concurrency upload + manifest-driven read/repair-ready facts”的第一阶段工业化版本。核心做法是复用现有 `TransferSession`、`TransferWritePlan`、`ChunkRef` manifest、`PlacementManager`、`StorageTransferClient` 和有界 runtime 边界，避免大规模重构和大量新结构体。

第一阶段目标不是完成全部对象存储长期能力，而是快速交付一组正确、可测、可继续扩展的核心行为：

- 每个 chunk 独立 placement，不固定 replica group。
- placement 资源感知，不按 `node_id` / name / config order 取前 N 个节点。
- upload 执行 CreateWritePlan 返回的 per-chunk selected replica nodes。
- 同 chunk replica fan-out 并行，chunk 成功由 `minimum_successful_writes` 决定。
- 多 chunk in-flight 受 `max_inflight_chunks` 和 `max_inflight_bytes` 同时限制。
- CommitObject 仍是对象可见性边界，manifest 保存实际 durable replica nodes。
- read path 按 committed manifest replica fallback。
- repair 方向采用决策 B：缺失副本重新 placement，不强制补回原节点。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: gRPC, Protobuf, GoogleTest, CMake, standard library  
**Storage**: StorageNode chunks under StorageNode data dir; metadata object manifest in existing metadata records; no payload in metadata/Raft  
**Testing**: GoogleTest + CTest + `./test.sh --group unit` / targeted storage tests  
**Target Platform**: Linux primary validation; Windows/macOS design-compatible and must not receive Linux-only shared business paths  
**Project Type**: Local RPC distributed object storage data-plane with Raft metadata control-plane  
**Performance Goals**: First stage supports bounded multi-chunk upload and per-chunk parallel replica fan-out; memory bounded by configured in-flight bytes, not file size  
**Constraints**: Preserve final CommitObject visibility boundary, protocol meaning, persisted manifest format, class/function names, and store/Raft separation unless a change is explicitly additive and covered by tests  
**Scale/Scope**: GB to hundred-GB class files, 9+ StorageNodes in tests, multiple chunks, retryable failures, partial write failures, manifest fallback reads  

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Verified existing capabilities affected by this feature are identified and excluded from unnecessary replanning.
  - PASS: Existing chunk durable write/read, metadata committed manifest, local RPC transfer, cleanup candidate and placement policy baselines are preserved.
- Any protocol, public API, or persisted format change is either absent or explicitly justified with migration and regression coverage.
  - PASS with scoped risk: Stage 1 should prefer extending existing C++ transfer facts and existing manifest fields. Any proto change must be additive, payload-free, field-number-safe, and covered by descriptor and adapter tests. Persisted manifest format should remain `ChunkRef`-based.
- Durability, crash-recovery, and restart-recovery implications are stated for every affected path in `node`, `replication`, `storage`, or `state_machine`.
  - PASS: No Raft durability or storage format change is planned. Chunk durable semantics remain in StorageNode data-plane. Failed uploads retain cleanup candidates and do not become visible without CommitObject.
- Linux-specific validation is explicitly labeled, and Windows/macOS fallback, adaptation, or deferred follow-up is recorded.
  - PASS: Stage 1 uses standard C++ concurrency and existing wrappers. Linux remains primary validation; shared implementation must compile without Linux-only calls.
- Test entry points are defined through CTest plus any justified platform-specific script or preset additions.
  - PASS: Tests are existing storage/placement/transfer CTest targets, with no new platform-specific script required.
- Observability and diagnostics impact is captured for high-risk work.
  - PASS: Plan requires diagnostics for placement epoch, selected replica nodes, fan-out errors, in-flight budget, fallback reads and cleanup/repair candidates.

## Current Code Baseline From Targeted Inspection

- `modules/store/transfer/object_transfer.cpp` implements the production `storage_client` upload/download path. Upload currently performs two bounded file passes: first pass computes per-chunk and object checksum facts, second pass writes chunks and calls `CommitObject`.
- `ObjectTransfer` session concurrency is currently explicitly clamped to one chunk in-flight. Larger CLI concurrency is diagnostic only, not a true pipeline.
- `ObjectTransfer` upload currently resolves chunk targets from write plan `candidate_nodes` if present, then silently fills missing targets from `SortedStorageTargets(...)`, which sorts by `node_id`.
- `ObjectTransfer` download currently validates committed manifest layout but selects the first discoverable manifest replica for each chunk instead of trying all same-chunk replicas.
- `MetadataTransferClient::CreateWritePlan(...)` currently maps to `CreateObject` and returns a `TransferWritePlan` without chunk placement.
- `proto/common.proto` already defines `ChunkRef { chunk_id, offset, size, replica_nodes, checksum }`, which is sufficient for committed manifest facts without payload.
- `modules/store/placement/replica_policy.cpp` filters by health, disk pressure, write admission and capacity, then sorts by available capacity, total inflight, writes, reads, then `node_id`. The last tie-break violates 010 production placement requirements.
- `modules/store/upload/upload_coordinator.*` already has lower-level upload orchestration and placement execution tests, but its current flow also chooses placement internally rather than consuming a returned write plan.
- `modules/store/maintenance` has GC/scrub/repair/rebalance foundations, but completed repair currently does not coordinate metadata manifest updates. Stage 1 will only make placement/manifest facts repair-ready.
- `apps/storage_client.cpp` still has 4MiB chunk size defaults and a config-file `chunk_size_bytes` read path. 010 requires a single code-level production chunk size default and no config-driven chunk size semantics.

## First-Stage Scope

The first stage is intentionally small and deliverable:

- Introduce one code-level production chunk size default, preferably `kProductionChunkSizeBytes = 128ULL * 1024ULL * 1024ULL`, in one shared store/common location and consume it from CLI/transfer defaults.
- Extend existing transfer write-plan facts so CreateWritePlan can express per-chunk selected replica nodes, chunk size, total chunks, replica policy, placement epoch and expiry without creating many new DTOs.
- Update placement ranking to remove lexical `node_id` / name / config-order priority and use resource-first ordering plus deterministic chunk-scoped jitter for equal-resource distribution.
- Populate per-chunk write plans by running placement per prepared chunk over current StorageNode facts; different chunks may receive different selected replica sets.
- Update upload execution to require selected replica nodes from the plan and fail explicitly if selected targets are missing, instead of filling from sorted discovery targets.
- Add per-chunk parallel replica fan-out with bounded fan-out work and `minimum_successful_writes` success semantics.
- Add bounded multi-chunk in-flight upload with `max_inflight_chunks`, `max_inflight_bytes`, and backpressure.
- Update production download to attempt manifest replicas in read-policy order with checksum verification and fallback before failing a chunk.

## Deferred Items

- **完整单遍流式 upload**: Deferred because it couples chunk checksum, object checksum, placement timing and CommitObject facts. Stage 1 keeps the two-pass model but ensures the second pass is bounded and concurrent.
- **完整 resumable upload session**: Deferred because it requires durable session state, idempotency expansion and cleanup/retry coordination beyond first-stage delivery.
- **完整 repair 决策 B 闭环**: Deferred because it needs local chunk inventory reporting, metadata manifest comparison, copy, verify and metadata manifest update. Stage 1 only guarantees manifest correctness and placement support for future repair.
- **完整 runtime tuning 工业化**: Deferred because hardware profiles, adaptive queues and maintenance scheduling would expand scope. Stage 1 provides minimal safe defaults and diagnostics.
- **大量 metrics 扩展**: Deferred to avoid turning first stage into observability breadth work. Stage 1 adds only diagnostics necessary to test and debug new behavior.
- **复杂 failure-domain 策略**: Deferred beyond current zone spread and resource filters. Stage 1 keeps deterministic and testable policy behavior.
- **server-side CreateWritePlan as a new RPC contract**: Deferred unless implementation proves existing transfer facts cannot carry the first-stage plan. If needed later, it must be additive and payload-free.

## Risk Analysis

- **Placement determinism vs hotspot avoidance**: Removing node-id tie-break while keeping tests deterministic requires a stable chunk-scoped jitter. The jitter may use `node_id` only as identity input to a hash, not as lexical priority or weight.
- **Plan authority ambiguity**: Current `CreateWritePlan` adapter only creates a pending metadata object. Stage 1 must make the logical write-plan boundary explicit so upload execution consumes selected nodes, even if the first implementation still composes metadata pending creation with placement module decisions.
- **Bounded concurrency correctness**: Multi-chunk concurrency can easily become unbounded if file reads continue while writes block. The implementation must acquire both chunk and byte budgets before reading payload into memory.
- **Fan-out cancellation semantics**: C++ standard futures do not forcibly cancel already-running RPCs. Stage 1 should stop waiting once enough durable successes are collected only where safe, while still aggregating or bounding outstanding work according to deadline and fan-out limit.
- **Manifest correctness after partial failure**: Durable chunks from failed uploads must remain cleanup candidates, not committed manifest facts. CommitObject must only receive chunks that met `minimum_successful_writes`.
- **Read fallback and corruption handling**: A checksum mismatch from one replica should not poison the whole object if another manifest replica is healthy, but the failed replica must be diagnosed for later repair.
- **Header impact**: Some first-stage changes require extending existing public C++ structs in `.h` files, especially transfer plan and session snapshot fields. The impact is limited to transfer/upload tests and adapters; complex logic remains in `.cpp`.
- **Cross-platform concurrency**: The plan must avoid Linux signals, epoll, platform thread APIs or Linux-only fsync assumptions in upload orchestration. Existing chunk durability remains under store/io and chunk modules.

## Technical Design

### Placement

- Keep `PlacementManager` as the policy coordinator and `ReplicaPolicySelector` as the pure strategy engine.
- Continue filtering unhealthy, read-only, draining, unavailable, degraded, high/full disk pressure, overloaded and capacity-insufficient nodes.
- Change equal-resource ordering from `node_id` to deterministic chunk-scoped jitter. The seed should be controllable for tests using existing request facts such as chunk identity and decision epoch, rather than adding a large new policy object.
- Preserve `node_id` in results for identity, RPC routing, manifest, diagnostics and repair facts.
- Require tests where identical node facts but shuffled IDs/config order do not collapse to "first replica_count nodes".

### Write Plan

- Reuse `TransferWritePlan` and `TransferChunkPlan`.
- Extend existing fields only where required, for example selected replica nodes, chunk size, total chunks, placement epoch and expiry. If field expansion is enough, add no new core struct.
- Treat `candidate_nodes` as optional fallback/candidate facts, not as the authoritative selected replica list after Stage 1.
- For each prepared chunk, run placement independently. The plan must preserve per-chunk selected nodes and `minimum_successful_writes`.
- If selected nodes cannot be produced for any chunk, CreateWritePlan fails before payload upload.

### Upload Execution

- Keep final CommitObject as the visibility boundary.
- Keep first-pass checksum preparation for Stage 1; do not attempt single-pass streaming in this feature.
- In the second pass, enforce `max_inflight_chunks` and `max_inflight_bytes` before reading the next chunk payload.
- For each in-flight chunk, issue writes to selected replica targets in parallel, bounded by fan-out concurrency.
- Mark chunk commit-eligible when durable successes reach `minimum_successful_writes`.
- Preserve failed, slow, uncertain and durable-but-uncommitted facts for diagnostics, cleanup candidates and later repair.
- Remove silent fallback to sorted discovery targets. Missing selected target is an explicit plan/discovery error.

### Metadata Manifest

- CommitObject receives only chunks that met `minimum_successful_writes`.
- Manifest stores actual durable replica nodes, not merely planned nodes.
- Existing `ChunkRef.replica_nodes` remains the committed source of truth.
- No payload enters metadata, Raft log, Raft snapshot or metadata snapshot.

### Read Path

- Download fetches committed manifest from MetadataNode and rejects pending/deleted/invisible objects.
- For each chunk, build the candidate list from that chunk's manifest replica nodes.
- Use read policy facts where available to order candidates, then try same-chunk replicas until checksum-verified success or all fail.
- Aggregate failed replica diagnostics and leave repair candidate facts for later workflows.

### Runtime Defaults

- Stage 1 defines minimal safe defaults:
  - production chunk size: 128 MiB from a single code-level store/common entry.
  - upload chunk concurrency: small bounded default, e.g. 2.
  - replica fan-out concurrency: bounded by replica count and a small default.
  - max in-flight bytes: at least one production chunk, preferably a small multiple of production chunk size.
- Runtime tuning must not auto-change `replica_count`, `minimum_successful_writes`, durability contract, consistency policy or commit visibility boundary.

### New Type Budget

- Target: zero new core public structs.
- Acceptable if unavoidable: one internal `.cpp` helper type for chunk upload task aggregation, with lifecycle ending when the upload session completes.
- Any new type must not expand the call chain across modules; it must only aggregate existing request/result facts for concurrent execution.

## Project Structure

### Documentation (this feature)

```text
specs/010-object-storage-config-industrialization/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
├── contracts/
│   ├── write-plan-and-manifest.md
│   ├── upload-concurrency.md
│   └── read-and-repair-direction.md
└── tasks.md
```

### Source Code (repository root)

```text
apps/
└── storage_client.cpp

modules/store/
├── common/
├── transfer/
├── placement/
├── upload/
├── runtime/
├── chunk/
└── maintenance/

modules/raft/
├── metadata/
├── service/
└── state_machine/

proto/
├── common.proto
└── metadata.proto

tests/
├── store_placement_policy_test.cpp
├── store_placement_manager_test.cpp
├── storage_upload_coordinator_test.cpp
├── storage_upload_integration_test.cpp
├── storage_read_integration_test.cpp
├── metadata_manifest_test.cpp
├── integrated_object_storage_concurrency_test.cpp
└── support/
```

**Structure Decision**: Primary implementation belongs in `modules/store/placement` and `modules/store/transfer`. `modules/store/upload` is touched only where lower-level coordinator tests must preserve the same semantics. `proto` and `modules/raft/service` should be touched only if additive plan expression cannot be carried in existing transfer facts.

## Acceptance Criteria

- Placement does not use `node_id`, node name or config order as production priority.
- With 9 StorageNodes and many chunks, chunk distribution covers most healthy nodes.
- Different chunks can receive different replica sets.
- Upload consumes CreateWritePlan selected replica nodes and has no silent fixed-order fallback.
- CreateWritePlan can express per-chunk selected replica nodes.
- Metadata manifest stores actual `chunk -> replica_nodes`.
- Same-chunk replica writes fan out in parallel.
- Upload supports bounded multi-chunk in-flight execution.
- `max_inflight_bytes` is enforced before reading more chunk payload.
- No unbounded file read or whole-object resident payload appears.
- Added core struct count remains zero unless one narrowly justified helper is unavoidable.
- Shared implementation remains cross-platform and introduces no Linux-only dependency.

## Minimal Test Plan

- `store_placement_policy_test`: resource-aware selection, no node-id lexical priority, deterministic jitter, 9-node multi-chunk distribution.
- `store_placement_manager_test`: registry snapshot filtering, selected node reasons, no silent fallback, repair-B excluded/current-facts placement behavior.
- `storage_upload_coordinator_test`: per-chunk plan consumption, actual durable replica manifest, fan-out success/failure aggregation.
- `storage_upload_integration_test`: multi-chunk upload, dynamic replica sets in committed manifest, final CommitObject visibility boundary.
- `integrated_object_storage_concurrency_test`: bounded `max_inflight_chunks`, bounded `max_inflight_bytes`, backpressure, parallel fan-out observation.
- `storage_read_integration_test`: committed manifest read fallback across same-chunk replicas and checksum validation.
- `metadata_manifest_test`: manifest remains payload-free and preserves per-chunk replica nodes across snapshot/restore.
- Build validation: `cmake --preset debug-ninja-low-parallel` and targeted CTest/test.sh groups.

## Post-Design Constitution Check

- Preserve verified core: PASS. Existing chunk durability, Metadata commit boundary, manifest payload-free records and Raft paths are preserved.
- Protocol/public API/persisted format: PASS with caution. Preferred Stage 1 path uses existing C++ transfer facts and `ChunkRef`. Any proto addition must be additive, payload-free and explicitly tested.
- Durability/recovery: PASS. Stage 1 does not weaken required durability operations. Partial durable writes remain cleanup/repair candidates until metadata commit.
- Cross-platform: PASS. Planned concurrency uses standard C++ and existing module boundaries.
- Observability/minimal surface: PASS. Diagnostics are limited to placement decisions, in-flight budget, fan-out result aggregation and read fallback.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Potential additive transfer/proto plan fields | CreateWritePlan must express per-chunk selected replicas | Continuing to infer targets in upload would preserve the current fixed-order fallback risk |
| Possible internal upload task aggregation helper | Parallel fan-out and multi-chunk completion need per-task result aggregation | Reusing a broad new public DTO would violate the no-many-struct constraint |
