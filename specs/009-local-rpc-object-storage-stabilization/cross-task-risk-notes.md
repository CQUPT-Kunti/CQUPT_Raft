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

## R13: Dynamic Metadata Join Has Batch-Promote Safety Coverage, But End-To-End Local RPC And Membership Traceability Still Lag

Risk: T055-T082 now cover dynamic Metadata candidate identity/config, `JoinMetadataCluster` contract, learner catch-up, learner exclusion from election/quorum, no-committed-`4-voter` safety, leader failover during batch promote, restart recovery of committed `5-voter` membership, and the `RaftNode` atomic batch promotion boundary. However, the repository still lacks end-to-end local RPC dynamic metadata add-node smoke and a dedicated first-class committed membership transition trace. Without those pieces, later storage/replay/diagnostics work could drift from the current targeted-test safety envelope.

Mitigation: Treat current US4 progress as targeted Linux validation of the core membership safety path. Keep Phase 10 local RPC workflow validation pending, and require later membership persistence / diagnostics work to preserve the same `3 voters -> 5 voters` atomicity and no-committed-`4-voter` guarantees already covered by the current tests.

## R19: Atomic Batch Promotion Boundary Exists, But First-Class Membership Persistence And History Still Lag Behind

Risk: T082 now provides a working atomic batch learner promotion boundary by encoding the transition as an internal Raft log command inside `RaftNode`, and targeted failover / restart tests can reach and recover a committed `5-voter` membership. However, committed membership persistence, replay traceability, and diagnostics still do not have a dedicated first-class membership-change channel. Later work on storage/replay/history may drift from the current internal-command boundary if T083/T085 do not preserve the same `3 voters -> 5 voters` atomicity guarantees.

Mitigation: Treat the T082 implementation as the current production safety boundary, not the final persistence model. T083 should preserve the same no-committed-`4-voter` invariant through storage/replay paths, and later diagnostics work should expose an explicit committed membership transition trace so failover/restart/history assertions do not depend only on sampled summaries and message strings.

## R20: Batch Promote Still Reuses JoinMetadataCluster Contract Instead Of A Dedicated Promote RPC

Risk: T083 wires batch promote through the existing `JoinMetadataCluster` service boundary by treating a duplicate request for a ready learner as the safe routing point into `RaftNode`'s atomic batch promotion boundary. This keeps changes minimal and preserves authority in `RaftNode`, but the proto still lacks a dedicated promote request/response contract, explicit promote-specific disposition codes, and first-class promote diagnostics fields. Future client, retry, or observability work could otherwise depend too heavily on free-form message strings and the overloaded meaning of `committed_membership_changed`.

Mitigation: Keep `JoinMetadataCluster` as the current minimal routing boundary for 009, but if later workflow or CLI needs a first-class operator-driven promote step, add an explicit promote contract instead of further overloading join semantics. Any future contract must preserve the existing no-committed-`4-voter` invariant and keep committed membership authority inside `RaftNode`.

## R21: Batch Promote Safety Is Closed By Targeted Linux Tests, But Runtime Workflow And Platform Coverage Still Lag

Risk: T086 now confirms the current `3 voters + 2 ready learners -> committed 5 voters` boundary on Linux targeted CTest, including no committed `4-voter` history, blocked single-learner promote, failover safety, restart recovery, and committed-voters-only quorum. However, the evidence is still scoped to targeted tests. The repository still lacks joint consensus, long-running failover / duplicate-request soak, Phase 10 local RPC dynamic metadata join + batch promote smoke, multi-ViewNode runtime observation/promote interaction coverage, and Windows/macOS real-machine validation.

Mitigation: Treat the current PASS as closure of the targeted US4 safety boundary, not as full runtime or cross-platform completion. Preserve the rule that ViewNode observation is never membership authority and that committed membership only changes through the Raft log / committed config path. Track local RPC workflow validation, broader failover soak, and Windows/macOS runs as separate follow-up work.

## R22: ViewNode Failover Can Be Falsely Marked Unavailable By Over-Strict Script Readiness Rules

Risk: `examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view` previously equated “surviving view ready” with strict cluster-shape checks. The first false negative was the fixed baseline counts `metadata_nodes=3` and `storage_nodes=6`, which broke after dynamic `store-7` join and `meta-4/meta-5` promotion even when the survivor-side snapshot already showed `target_endpoint=127.0.0.1:8302`, `view-2 liveness=live`, and `view-2 health=healthy`. A second false negative remained when failover happened during partial registry convergence: `status OK` could still show `metadata_nodes=3` but `storage_nodes=0`, and the script still returned `surviving_view_status_unavailable` even though the surviving ViewNode itself was alive and serving status/discovery.

Mitigation: Validate failover readiness by survivor endpoint + survivor self live/not-unavailable status + non-authority boundary + surviving metadata discovery, while explicitly allowing degraded / partial storage observations. Keep `peer sync` connection-refused/backoff diagnostics visible, but do not let them imply self unavailable. Guard this with `ViewFailoverScriptValidation`.

## R23: Example Runtime State Leakage Can Pollute Dynamic Join Reruns

Risk: Re-running the 009 local RPC example without resetting runtime state can leave previously promoted committed membership in example data directories. A later “fresh” rerun may then fail at `join-metadata-learner` with diagnostics such as `candidate_raft_id already exists in committed voter set`, even though the current fix is unrelated to metadata join logic. This can block clean full-sequence validation and confuse Phase 10/T098 follow-up work.

Mitigation: Treat example-node data directories as runtime state, not source truth. Before claiming a clean full-sequence rerun, reset the example runtime state or use a fresh data root so learner-join and batch-promote validation start from the intended initial `3`-voter membership.

## R24: Persisted Registry Recovery Test Now Covers Merge Semantics, But Runtime Durable Load Must Reuse The Same Boundary

Risk: T104 now validates the restart path by restoring an exported ViewNode registry snapshot and then reconverging through peer sync. This proves the incarnation-aware merge and eventual-convergence semantics, but it does not by itself guarantee that a future file-backed or app-startup durable load path will apply the exact same snapshot shape, restore order, and stale-state rejection rules. A later runtime persistence implementation could still drift and reintroduce rollback or long-lived divergence bugs even though the current targeted test passes.

Mitigation: Any future runtime registry persistence/load path should reuse the same observed-state snapshot model and import ordering already covered by T104: restore local snapshot first, preserve restarted self state by incarnation/sequence ordering, then reconverge through peer sync. Do not introduce a second “authoritative” recovery path or a different merge contract for persisted registry data.

## R25: Metadata Recovery Stress Still Shows Occasional Timing Sensitivity Under Broad Regression

Risk: During T109 validation, the first broad `ctest --preset debug-tests -R "Metadata|NodeIdentity" --output-on-failure` run failed once in `MetadataRecoveryStressTest.RestartRecoveryAfterConcurrentWritesKeepsCommittedAndDeletedMetadataStable`, while the focused rerun and the subsequent full broad rerun both passed. This suggests a residual timing-sensitive recovery stress path outside the T109 identity-boundary changes. It does not currently prove a deterministic logic regression, but it can still block future phase-closure tasks if left invisible.

Mitigation: Treat current T109 PASS as valid because the focused T109 scenarios and the final broad rerun both passed, but keep this recovery-stress flake explicit. Later phase-closure work should either stabilize that recovery stress test or document its timing envelope so broad metadata regressions do not fail nondeterministically.
