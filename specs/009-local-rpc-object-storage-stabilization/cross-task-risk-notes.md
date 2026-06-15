# Cross-Task Risk Notes: 009 Local RPC Object Storage Stabilization

## R1: ViewNode Peer Sync Can Accidentally Become Membership Authority

Risk: Registry sync may expose metadata roles and leader hints, tempting later implementation to treat ViewNode state as Raft membership.

Mitigation: Keep contracts explicit: ViewNode observed state is diagnostic/discovery only. Raft committed configuration log is the only voter/learner authority.

## R2: observed_time-Only Merge Can Reintroduce Old-Incarnation Bugs

Risk: Peer snapshots and delayed heartbeats can arrive with later wall-clock timestamps but older process incarnation.

Mitigation: Merge by incarnation first, then sequence. Use `observed_time` only for TTL/liveness after ordering permits the update.

## R3: Registry Persistence Needs Versioning And Recovery Boundaries

Risk: Persisting ViewNode registry without snapshot version or incarnation semantics can make stale state survive restarts incorrectly.

Mitigation: Either version registry snapshots with deterministic merge safety, or document memory-only restart recovery and rely on self refresh/heartbeat/peer sync rehydration.

## R4: Learner Promote Can Violate Odd Voter Invariant

Risk: Existing single-member membership change patterns may naturally implement 3 -> 4 -> 5 voters, which 009 forbids.

Mitigation: Block single learner promote when target voter count is even. Implement batch promote / joint consensus before allowing 3 + 2 learners -> 5 voters.

## R5: Dynamic Metadata Join Crosses Many High-Risk Modules

Risk: Join touches `proto/`, `modules/raft/service`, `modules/raft/node`, `modules/raft/replication`, storage/restart tests, and app startup.

Mitigation: Keep dynamic join scoped. Validate with Raft unit tests before local RPC end-to-end tests. Do not alter existing static bootstrap behavior except where explicitly tested.

## R6: Identity Changes Can Break Existing Static Example

Risk: Tightening identity validation may reject current `examples/object-storage-local-3meta-6store` layout.

Mitigation: Treat 008 example as preservation baseline. Missing `identity_file` on first start must create the current new-format identity; mismatch, corrupt, old-format, and missing-required-field cases must fail fast instead of auto-upgrading.

## R8: Legacy Identity Compatibility Can Mask Invalid Node Identity

Risk: Keeping silent v1/legacy compatibility can hide missing required fields, corrupt files, or invalid membership semantics by auto-filling defaults during load.

Mitigation: 009 is pre-deployment for the new identity schema. Support only the current `node.identity` format. Existing malformed, old-format, or missing-required-field identity files must fail fast and must never auto-upgrade or auto-overwrite.

## R7: Test Scope Can Collapse Into Happy Path

Risk: Dynamic registration could pass one local demo while missing restart, duplicate, TTL, stale heartbeat, old incarnation, leader failover, and odd-voter tests.

Mitigation: Keep validation matrix mandatory for phase closure. Every phase task should name its CTest/example entry and failure class.

## R9: Identity Atomic Publish Durability Is Only Logically Validated On Linux

Risk: `node.identity` atomic publish currently has Linux-targeted logic coverage for `fsync(fd) -> atomic publish -> fsync(data_dir)` and explicit Windows refusal for required directory durability, but the project has not run power-loss-grade validation or real Windows/macOS durability confirmation in 009.

Mitigation: Keep Linux as the primary validated platform for now, record Windows/macOS as pending, and do not claim cross-platform durability PASS until dedicated platform validation runs. If required durability cannot be proven on a platform, the implementation must keep returning an explicit error rather than silently succeeding.

## R10: Incarnation-Aware Merge Ordering Is Validated Only On Single-View RPC Paths

Risk: T026-T033 now validate higher-incarnation wins, same-incarnation higher-sequence wins, conflict diagnostics, and service/client adapter mapping on single-ViewNode registry paths, but peer-sync network propagation and multi-ViewNode active-active convergence are still not implemented. A later peer-sync path could still drop `incarnation_id`, `sequence`, or stale-diagnostic information and reintroduce old-state overwrite bugs across ViewNodes.

Mitigation: Treat current Linux PASS as single-View scope only. Keep peer-sync and multi-View failover validation explicitly pending for Phase 5. Any new peer snapshot/push-pull RPC must preserve incarnation-aware observed-state ordering and must not collapse ViewNode registry into membership authority.

## R11: Multi-View Peer Sync Still Lacks Soak And Non-Linux Runtime Validation

Risk: T035-T043 complete the Linux targeted functional path for dual ViewNode peer sync, but they do not prove long-running convergence, repeated disconnect/retry stability, or Windows/macOS runtime behavior. A later regression could hide in retry/backoff timing or platform-specific networking/thread shutdown paths.

Mitigation: Keep Linux targeted PASS scoped to `ViewNodeDiscovery` functional coverage. Record Windows/macOS as pending, keep local RPC multi-View smoke and soak validation as follow-up work, and do not claim cross-platform peer-sync runtime stability before those checks exist.

## R12: Dynamic StorageNode Join Still Lacks Real Local RPC Add-Node Example Validation

Risk: T045-T053 now validate StorageNode dynamic join semantics through registry tests, placement integration tests, transfer-path compatibility, and metadata manifest diagnostics, but the repository still lacks a completed local RPC example command that adds a new StorageNode to a running multi-process cluster and demonstrates the full runtime path end to end. Without that example, Linux PASS remains scoped to targeted test harnesses rather than the final example workflow.

Mitigation: Keep current US3 PASS scoped to `storage_heartbeat_registry` and `IntegratedObjectStorageE2ETest.*`. Add real runtime add-node example commands and smoke validation in Phase 10/T089-T098 before claiming full local RPC dynamic-join workflow validation.

## R13: Dynamic Metadata Join Still Stops At Validation And Admission Boundary

Risk: T055-T065 now validate dynamic Metadata candidate identity/config, `JoinMetadataCluster` proto contract, metadata leader authority, ViewNode discovery fallback, and `AddLearner` admission boundary, but the runtime still stops before committed learner membership change, learner catch-up, InstallSnapshot catch-up, promote-to-voter, odd-voter-safe promotion, and local RPC dynamic metadata add-node smoke. Multi-ViewNode peer sync effects on metadata join discovery are also not fully exercised beyond targeted tests.

Mitigation: Keep current US4 PASS scoped to validation/boundary semantics only. Treat T066-T098 as required follow-up for learner replication, snapshot catch-up, promotion safety, local RPC runtime smoke, and broader multi-ViewNode discovery validation before claiming full dynamic Metadata membership lifecycle support.

## R14: Learner Catch-Up Phase Is Linux-Validated, But Promotion And Cross-Platform Runtime Validation Remain Pending

Risk: T066-T076 now cover learner AppendEntries catch-up, InstallSnapshot catch-up, learner exclusion from election/quorum, committed-voters-only quorum safety, and waiting-for-pair diagnostics on Linux targeted tests, but the repository still lacks promote-to-voter, batch promote, joint consensus, and cross-platform runtime validation. Treating Phase 8 PASS as full learner lifecycle completion would overstate the current safety envelope.

Mitigation: Keep learner Phase 8 closure scoped to Linux-targeted catch-up and non-voter safety semantics only. Record Windows/macOS as pending, require T078+ to validate batch promotion and no-committed-4-voter history, and do not claim end-to-end dynamic metadata learner lifecycle completion before those tasks and local RPC smoke are finished.

## R15: T078 Cannot Yet Reach Two Ready Learners Or Prove No-Committed-4-Voter History Through Real APIs

Risk: Current runtime membership admission still stores only one `pending_add_learner_proposal_`, and the public path exposed by `JoinMetadataCluster` has no batch promote / promote-to-voter boundary. As a result, the T078 test can drive one learner to `ready_to_promote` and verify `waiting_for_pair`, but it cannot yet continue through a real `3 voters + 2 ready learners -> direct 5 voters` transition or prove the absence of committed 4-voter intermediate history with executable assertions.

Mitigation: Keep T078 as an intentionally red test-first guard until production exposes a real second-learner admission path plus an explicit batch promote boundary. When that boundary exists, extend the test to verify: before promote both learners remain non-voters and quorum stays at 2; after batch promote committed voters become 5 with quorum 3; and no committed 4-voter membership is ever observable in diagnostics or state summaries.
