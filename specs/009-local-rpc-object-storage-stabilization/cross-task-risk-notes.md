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

Mitigation: Keep dynamic join additive. Validate with Raft unit tests before local RPC end-to-end tests. Do not alter existing static bootstrap behavior except where explicitly tested.

## R6: Identity Changes Can Break Existing Static Example

Risk: Tightening identity validation may reject current `examples/object-storage-local-3meta-6store` layout.

Mitigation: Treat 008 example as compatibility baseline. Missing `identity_file` on first start must create identity; mismatch/corrupt cases fail fast only when truly invalid.

## R7: Test Scope Can Collapse Into Happy Path

Risk: Dynamic registration could pass one local demo while missing restart, duplicate, TTL, stale heartbeat, old incarnation, leader failover, and odd-voter tests.

Mitigation: Keep validation matrix mandatory for phase closure. Every phase task should name its CTest/example entry and failure class.

