# Validation Matrix: 009 Local RPC Object Storage Stabilization

## Baseline Matrix

| Area | Existing Entry | 009 Validation Goal | Notes |
|------|----------------|---------------------|-------|
| Local RPC example | `examples/object-storage-local-3meta-6store/qidong.sh`, `examples/object-storage-local-3meta-6store/rpc_demo.sh status`, `examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`, `examples/object-storage-local-3meta-6store/tingzhi.sh` | Preserve 008 static 1 ViewNode + 3 MetadataNode + 6 StorageNode real roundtrip as the 009 local RPC preservation baseline | Client remains `storage_client`; test file directory remains `tests/test_file`; report confirms `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp` |
| App targets | `view_node_app`, `metadata_node_app`, `storage_node_app`, `storage_client`, `raft_metadata_client` | Build only touched executable targets before example checks | Do not default to full build |
| ViewNode registry | `tests/view_node_discovery_test.cpp` | Self refresh, TTL, peer sync, incarnation-aware merge | Linux targeted validation through T034 covers self refresh, TTL, incarnation-aware ordering, adapter mapping, and conflict diagnostics on single-ViewNode paths; multi-ViewNode peer sync runtime remains pending |
| Storage heartbeat | `tests/storage_heartbeat_registry_test.cpp` | Dynamic register, heartbeat content, stale heartbeat, duplicate register | Storage join is discovery-only |
| Identity | `tests/node_identity_test.cpp` | First create, restart reuse, mismatch, corruption, type-specific Metadata rules | Missing identity_file is valid first-start input; existing legacy/old-format or missing-required-field identity files must fail fast with no auto-upgrade and no auto-overwrite |
| Cluster config | `tests/cluster_config_test.cpp` | Config boundaries for view/storage/metadata and odd initial voters | Dynamic nodes should not require full topology |
| Object E2E | `tests/integrated_object_storage_e2e_test.cpp` | Running-time StorageNode join then future upload/download | Existing objects need no rebalance |
| Quorum/membership | `tests/integrated_object_storage_quorum_test.cpp` | committed voters only, odd voter invariant, learner excluded from quorum | 3 voters + 1 learner quorum remains 2 |
| Raft election | `tests/test_raft_election.cpp` | learner cannot vote or become leader | committed voters only |
| Raft replication | `tests/test_raft_log_replication.cpp` | learner receives AppendEntries and advances match_index | Non-voter replication path |
| Raft snapshot catch-up | `tests/test_raft_snapshot_catchup.cpp` | learner receives InstallSnapshot after log compaction | Required for dynamic join |
| Raft restart | `tests/test_raft_snapshot_restart.cpp` | committed membership and learner state recover safely | No local-only voter promotion |
| Metadata failover | `tests/metadata_failover_test.cpp` | leader change during join/catch-up recovers or aborts safely | No inconsistent membership |
| Metadata client scenario | `tests/metadata_client_scenario_test.cpp` | leader discovery still handles NOT_LEADER and retries | ViewNode hints are not authority |

## Phase 1 CTest / Target / Label Confirmation

说明：

- `tests/CMakeLists.txt` 里当前有两类测试入口：`add_store_ctest()` 生成精确的 CTest test name；`add_raft_gtest()` / `gtest_discover_tests()` 生成 gtest case 级别的 CTest test name。
- 因此 `test_view_node_discovery`、`test_node_identity`、`cluster_config_test`、`test_integrated_object_storage_e2e`、`test_integrated_object_storage_quorum`、`test_raft_election` 等名称在当前仓库里首先是 build target，不等于最终 CTest case name。
- Phase 1 后续任务应区分：
  - targeted build：使用 executable/custom target。
  - targeted CTest：优先使用真实 CTest case regex 或确认过的 label。
- 仅做文本核对和 CTest listing；未执行 configure/build/test。

| 009 Area | Test File | Build Target | 真实 CTest 入口 | Custom Target | 已确认 Label | 后续任务建议入口 |
|----------|-----------|--------------|-----------------|---------------|---------------|------------------|
| ViewNode registry / self refresh / peer sync | `tests/view_node_discovery_test.cpp` | `test_view_node_discovery` | `ViewNodeDiscoveryTest.*` | 未确认到 | `integrated-object-storage`, `view-node`, `platform-neutral` | T018-T044、T099：build 用 `test_view_node_discovery`；CTest 用 `-L view-node` 或 `-R '^ViewNodeDiscoveryTest\\.'` |
| Identity lifecycle | `tests/node_identity_test.cpp` | `test_node_identity` | `NodeIdentityTest.*` | 未确认到 | `integrated-object-storage`, `node-identity`, `platform-neutral`, `durability-boundary`, `windows-adaptation` | T006-T017、T029、T101：build 用 `test_node_identity`；CTest 用 `-L node-identity` 或 `-R '^NodeIdentityTest\\.'` |
| Cluster config | `tests/cluster_config_test.cpp` | `cluster_config_test` | `cluster_config_generation_test.*`, `cluster_config_validation_test.*`, `cluster_config_endpoint_allocation_test.*`, `cluster_config_resolution_test.*`, `cluster_config_quorum_helper_test.*` | 未确认到 | `integrated-object-storage`, `platform-neutral` | T008、T016、T038：build 用 `cluster_config_test`；CTest 用 `-R '^cluster_config_'` |
| StorageNode heartbeat / registry | `tests/storage_heartbeat_registry_test.cpp` | `test_storage_heartbeat_registry` | `storage_heartbeat_registry` | 未确认到 | `storage-node`, `platform-neutral` | T045-T047、T049-T050、T100：build 用 `test_storage_heartbeat_registry`；CTest 可直接用 `-R '^storage_heartbeat_registry$'` |
| Integrated object storage e2e | `tests/integrated_object_storage_e2e_test.cpp` | `test_integrated_object_storage_e2e` | `IntegratedObjectStorageE2ETest.*` | `integrated_object_storage_e2e` | `integrated-object-storage`, `integrated-object-storage-e2e`, `storage-transfer`, `platform-neutral`, `linux-primary-diagnosis` | T048、T051、T089-T098：build 可用 `test_integrated_object_storage_e2e` 或 `integrated_object_storage_e2e`；CTest 用 `-L integrated-object-storage-e2e` 或 `-R '^IntegratedObjectStorageE2ETest\\.'` |
| Integrated object storage quorum | `tests/integrated_object_storage_quorum_test.cpp` | `test_integrated_object_storage_quorum` | `IntegratedObjectStorageQuorumTest.*` | `integrated_object_storage_quorum` | `integrated-object-storage`, `integrated-object-storage-quorum`, `platform-neutral` | T069-T070、T078-T086、T104-T105、T114：build 可用 `test_integrated_object_storage_quorum` 或 `integrated_object_storage_quorum`；CTest 用 `-L integrated-object-storage-quorum` 或 `-R '^IntegratedObjectStorageQuorumTest\\.'` |
| Integrated object storage recovery | `tests/integrated_object_storage_recovery_test.cpp` | `test_integrated_object_storage_recovery` | `integrated_object_storage_recovery.IntegratedObjectStorageRecoveryTest.*` | `integrated_object_storage_recovery` | `integrated-object-storage`, `integrated-object-storage-recovery`, `storage-transfer`, `storage-node`, `storage-node-recovery`, `durability-boundary`, `linux-primary-diagnosis` | 009 recovery / cleanup 相关回归：build 可用 `test_integrated_object_storage_recovery` 或 `integrated_object_storage_recovery`；CTest 用 `-L integrated-object-storage-recovery` 或 `-R '^integrated_object_storage_recovery\\.'` |
| Integrated object storage concurrency | `tests/integrated_object_storage_concurrency_test.cpp` | `test_integrated_object_storage_concurrency` | `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.*` | `integrated_object_storage_concurrency` | `integrated-object-storage`, `integrated-object-storage-concurrency`, `storage-transfer`, `storage-node`, `storage-node-concurrency`, `platform-neutral`, `linux-primary-diagnosis` | 009 concurrency / stress 相关回归：build 可用 `test_integrated_object_storage_concurrency` 或 `integrated_object_storage_concurrency`；CTest 用 `-L integrated-object-storage-concurrency` 或 `-R '^integrated_object_storage_concurrency\\.'` |
| Raft election | `tests/test_raft_election.cpp` | `test_raft_election` | `RaftElectionTest.*` | 未确认到 | `platform-neutral` | T068、T073、T104：build 用 `test_raft_election`；CTest 用 `-R '^RaftElectionTest\\.'` |
| Raft replication | `tests/test_raft_log_replication.cpp` | `test_raft_log_replication` | `RaftLogReplicationTest.*` | 未确认到 | `platform-neutral` | T066、T074、T077：build 用 `test_raft_log_replication`；CTest 用 `-R '^RaftLogReplicationTest\\.'` |
| Raft snapshot catch-up | `tests/test_raft_snapshot_catchup.cpp` | `test_raft_snapshot_catchup` | `RaftSnapshotCatchupTest.*` | 未确认到 | `platform-neutral` | T067、T075、T077：build 用 `test_raft_snapshot_catchup`；CTest 用 `-R '^RaftSnapshotCatchupTest\\.'` |
| Raft restart / snapshot recovery | `tests/test_raft_snapshot_restart.cpp` | `test_raft_snapshot_restart` | `RaftSnapshotRestartTest.*`, `RaftSnapshotRecoveryTest.*` | 未确认到 | `platform-neutral`, `durability-boundary`, `linux-specific-failure-injection` | T075、T081、T083、T088：build 用 `test_raft_snapshot_restart`；CTest 用 `-R '^(RaftSnapshotRestartTest|RaftSnapshotRecoveryTest)\\.'` |
| Metadata failover | `tests/metadata_failover_test.cpp` | `test_metadata_failover` | `MetadataFailoverTest.*` | 未确认到 | `platform-neutral` | T080、T086、T103：build 用 `test_metadata_failover`；CTest 用 `-R '^MetadataFailoverTest\\.'` |
| Metadata client scenario | `tests/metadata_client_scenario_test.cpp` | `test_metadata_client_scenario` | `MetadataClientScenarioTest.*` | 未确认到 | `platform-neutral` | T102：build 用 `test_metadata_client_scenario`；CTest 用 `-R '^MetadataClientScenarioTest\\.'` |

## Label Confirmation

| Label | `tests/CMakeLists.txt` 现状 | 绑定说明 |
|-------|-----------------------------|----------|
| `integrated-object-storage` | 已确认到 | `RAFT_008_LABELS_BASELINE/E2E/QUORUM/RECOVERY/CONCURRENCY/VIEW_NODE/NODE_IDENTITY` 均包含 |
| `integrated-object-storage-e2e` | 已确认到 | `test_integrated_object_storage_e2e` |
| `integrated-object-storage-quorum` | 已确认到 | `test_integrated_object_storage_quorum` |
| `integrated-object-storage-recovery` | 已确认到 | `test_integrated_object_storage_recovery` |
| `integrated-object-storage-concurrency` | 已确认到 | `test_integrated_object_storage_concurrency` |
| `view-node` | 已确认到 | `test_view_node_discovery` |
| `node-identity` | 已确认到 | `test_node_identity` |
| `storage-node` | 已确认到 | `storage_heartbeat_registry` 以及 object-storage recovery/concurrency 等 store 相关入口 |
| `platform-neutral` | 已确认到 | 覆盖 ViewNode、identity、cluster config、quorum、raft election/replication/snapshot catch-up 等多类入口 |
| `linux-primary-diagnosis` | 已确认到 | `test_integrated_object_storage_e2e`、`test_integrated_object_storage_recovery`、`test_integrated_object_storage_concurrency` 及部分 store/diagnosis 入口 |

## Phase 1 Notes

- `ctest --preset debug-ninja-low-parallel -N` 未确认到对应 test preset；当前仓库存在的 listing preset 为 `debug-tests`。
- 当前 `build/linux` 已存在，可用 `ctest --preset debug-tests -N` 做只列测试的轻量确认；本任务未为 T003 触发任何 configure/build。
- 当前请求中列出的重点 label 均已在 `tests/CMakeLists.txt` 中确认到；本节没有“未确认到”的重点 label。
- 当前请求中列出的重点 custom target 只确认到：
  - `integrated_object_storage_e2e`
  - `integrated_object_storage_quorum`
  - `integrated_object_storage_recovery`
  - `integrated_object_storage_concurrency`
- `test_view_node_discovery`、`test_node_identity`、`cluster_config_test`、`test_raft_election`、`test_raft_log_replication`、`test_raft_snapshot_catchup`、`test_raft_snapshot_restart`、`test_metadata_failover`、`test_metadata_client_scenario` 在当前 CMake 里是 executable target，不是 custom target。

## Scenario Matrix

| Scenario | Test Or Script Entry | Pass Criteria |
|----------|----------------------|---------------|
| Static local RPC baseline preservation | `examples/object-storage-local-3meta-6store/qidong.sh`, `examples/object-storage-local-3meta-6store/rpc_demo.sh status`, `examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`, `examples/object-storage-local-3meta-6store/tingzhi.sh` | 1 ViewNode + 3 MetadataNode + 6 StorageNode static real RPC roundtrip remains valid; later 009 scenarios must extend from this baseline and must not regress it |
| Local RPC ViewNode self-liveness regression | `examples/object-storage-local-3meta-6store/qidong.sh`, `examples/object-storage-local-3meta-6store/rpc_demo.sh status-self-liveness`, `examples/object-storage-local-3meta-6store/tingzhi.sh` | `storage_client status` 输出必须包含本地 ViewNode 自身记录及 `liveness`；跨过 dead TTL 后健康运行中的 ViewNode 仍应保持 `LIVE`，在 T021 之前若出现 `STALE` / `SUSPECT` / `DEAD` 应直接暴露失败 |
| Single ViewNode self refresh | `tests/view_node_discovery_test.cpp` | ViewNode remains `LIVE` for at least 2x dead TTL while self refresh is enabled |
| Self refresh stopped | `tests/view_node_discovery_test.cpp` | State transitions to `STALE` / `SUSPECT` / `DEAD` by TTL |
| Single-View incarnation-aware merge ordering | `tests/view_node_discovery_test.cpp` | Higher incarnation wins, same-incarnation higher sequence wins, `observed_time` alone cannot override newer live state; service/client adapter still exposes incarnation/sequence facts |
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
- Phase 1 baseline confirmation may be document-only; when scripts are not executed, record `未执行脚本，原因：本任务为 Phase 1 文档基线确认`.
- Skipped build/test due to build lock or missing platform must be recorded in `task-reports/`.
- Windows/macOS without real environment should be marked pending, not assumed passed.
