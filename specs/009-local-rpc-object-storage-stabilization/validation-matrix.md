# Validation Matrix: 009 Local RPC Object Storage Stabilization

## Baseline Matrix

| Area | Existing Entry | 009 Validation Goal | Notes |
|------|----------------|---------------------|-------|
| Local RPC example | `examples/object-storage-local-3meta-6store/qidong.sh`, `rpc_demo.sh status`, `rpc_demo.sh roundtrip`, `tingzhi.sh` | Preserve 008 static 1 ViewNode + 3 Metadata + 6 StorageNode real roundtrip | Report confirms `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp` |
| App targets | `view_node_app`, `metadata_node_app`, `storage_node_app`, `storage_client`, `raft_metadata_client` | Build only touched executable targets before example checks | Do not default to full build |
| ViewNode registry | `tests/view_node_discovery_test.cpp` | Self refresh, TTL, peer sync, incarnation-aware merge | Must cover old incarnation / newer observed_time conflict |
| Storage heartbeat | `tests/storage_heartbeat_registry_test.cpp` | Dynamic register, heartbeat content, stale heartbeat, duplicate register | Storage join is discovery-only |
| Identity | `tests/node_identity_test.cpp` | First create, restart reuse, mismatch, corruption, type-specific Metadata rules | Missing identity_file is valid first-start input |
| Cluster config | `tests/cluster_config_test.cpp` | Config boundaries for view/storage/metadata and odd initial voters | Dynamic nodes should not require full topology |
| Object E2E | `tests/integrated_object_storage_e2e_test.cpp` | Running-time StorageNode join then future upload/download | Existing objects need no rebalance |
| Quorum/membership | `tests/integrated_object_storage_quorum_test.cpp` | committed voters only, odd voter invariant, learner excluded from quorum | 3 voters + 1 learner quorum remains 2 |
| Raft election | `tests/test_raft_election.cpp` | learner cannot vote or become leader | committed voters only |
| Raft replication | `tests/test_raft_log_replication.cpp` | learner receives AppendEntries and advances match_index | Non-voter replication path |
| Raft snapshot catch-up | `tests/test_raft_snapshot_catchup.cpp` | learner receives InstallSnapshot after log compaction | Required for dynamic join |
| Raft restart | `tests/test_raft_snapshot_restart.cpp` | committed membership and learner state recover safely | No local-only voter promotion |
| Metadata failover | `tests/metadata_failover_test.cpp` | leader change during join/catch-up recovers or aborts safely | No inconsistent membership |
| Metadata client scenario | `tests/metadata_client_scenario_test.cpp` | leader discovery still handles NOT_LEADER and retries | ViewNode hints are not authority |

## Scenario Matrix

| Scenario | Test Or Script Entry | Pass Criteria |
|----------|----------------------|---------------|
| Single ViewNode self refresh | `tests/view_node_discovery_test.cpp` | ViewNode remains `LIVE` for at least 2x dead TTL while self refresh is enabled |
| Self refresh stopped | `tests/view_node_discovery_test.cpp` | State transitions to `STALE` / `SUSPECT` / `DEAD` by TTL |
| Dual ViewNode sync | ViewNode discovery/peer sync test | State registered on one ViewNode appears on peer after sync |
| Old incarnation rejected | ViewNode discovery/peer sync test | Older incarnation cannot override newer incarnation even with newer observed_time |
| ViewNode failover | local RPC example or integration test | Client discovers metadata/storage through surviving ViewNode |
| StorageNode first start | `tests/node_identity_test.cpp`, storage dynamic join test | identity_file created, node_id persisted |
| StorageNode restart | `tests/storage_heartbeat_registry_test.cpp` | Same node_id, new incarnation, stale old updates rejected |
| StorageNode dynamic placement | `tests/integrated_object_storage_e2e_test.cpp`, local RPC example | New object can use newly joined StorageNode |
| Metadata dynamic join candidate | `tests/node_identity_test.cpp`, metadata join test | Candidate starts joining/non-voter, not voter |
| AddLearner committed | Raft membership/join test | New metadata member becomes learner only through committed log |
| Learner log catch-up | `tests/test_raft_log_replication.cpp` | learner receives AppendEntries and advances progress |
| Learner snapshot catch-up | `tests/test_raft_snapshot_catchup.cpp` | learner receives InstallSnapshot and applies state |
| Single learner blocked | `tests/integrated_object_storage_quorum_test.cpp` | 3 voters + 1 ready learner returns blocked/waiting status; no 4 voters |
| Learner excluded from election | `tests/test_raft_election.cpp` | learner cannot vote or lead |
| Batch promote | quorum/membership batch test | 3 voters + 2 ready learners become 5 voters; quorum becomes 3; no committed 4 voters |
| Leader failover during join | `tests/metadata_failover_test.cpp` | new leader resumes or aborts pending join without divergent membership |

## Validation Reporting

- PASS report: command, PASS, elapsed time.
- Failure report: failed test name, key assertion, failure category, last 50 log lines, full log path.
- Skipped build/test due to build lock or missing platform must be recorded in `task-reports/`.
- Windows/macOS without real environment should be marked pending, not assumed passed.

