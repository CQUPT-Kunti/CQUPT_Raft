# Validation Matrix: 009 Local RPC Object Storage Stabilization

## Baseline Matrix

| Area | Existing Entry | 009 Validation Goal | Notes |
|------|----------------|---------------------|-------|
| Local RPC example | `examples/object-storage-local-3meta-6store/qidong.sh`, `examples/object-storage-local-3meta-6store/rpc_demo.sh status`, `examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`, `examples/object-storage-local-3meta-6store/tingzhi.sh` | Preserve 008 static 1 ViewNode + 3 MetadataNode + 6 StorageNode real roundtrip as the 009 local RPC preservation baseline | Client remains `storage_client`; test file directory remains `tests/test_file`; report confirms `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp` |
| App targets | `view_node_app`, `metadata_node_app`, `storage_node_app`, `storage_client`, `raft_metadata_client` | Build only touched executable targets before example checks | Do not default to full build |
| ViewNode registry | `tests/view_node_discovery_test.cpp`, `tests/view_failover_test.cpp` | Self refresh, TTL, peer sync, incarnation-aware merge, failover, recovery, convergence | Current runtime registry recovery boundary remains memory-only: Linux targeted validation covers self refresh, TTL, incarnation-aware ordering, adapter mapping, peer sync background convergence, failover, restored-snapshot merge safety, and restart reconvergence on observed-state paths |
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
| ViewNode registry / self refresh / peer sync | `tests/view_node_discovery_test.cpp` | `test_view_node_discovery` | `ViewNodeDiscoveryTest.*` | 未确认到 | `integrated-object-storage`, `view-node`, `platform-neutral` | T018-T044、T105：build 用 `test_view_node_discovery`；CTest 用 `-L view-node` 或 `-R '^ViewNodeDiscoveryTest\\.'` |
| ViewNode failover survivor availability regression | `tests/view_failover_test.cpp` | `view_failover_test` | `ViewFailoverTest.*` | 未确认到 | `integrated-object-storage`, `view-node`, `platform-neutral` | T100-T105：build 用 `view_failover_test`；CTest 用 `-R '^ViewFailoverTest\\.'` 或 `-R 'ViewFailover|FailoverView|ViewNode'` |
| Identity lifecycle | `tests/node_identity_test.cpp` | `test_node_identity` | `NodeIdentityTest.*` | 未确认到 | `integrated-object-storage`, `node-identity`, `platform-neutral`, `durability-boundary`, `windows-adaptation` | T006-T017、T029：build 用 `test_node_identity`；CTest 用 `-L node-identity` 或 `-R '^NodeIdentityTest\\.'` |
| Cluster config | `tests/cluster_config_test.cpp` | `cluster_config_test` | `cluster_config_generation_test.*`, `cluster_config_validation_test.*`, `cluster_config_endpoint_allocation_test.*`, `cluster_config_resolution_test.*`, `cluster_config_quorum_helper_test.*` | 未确认到 | `integrated-object-storage`, `platform-neutral` | T008、T016、T038：build 用 `cluster_config_test`；CTest 用 `-R '^cluster_config_'` |
| StorageNode heartbeat / registry | `tests/storage_heartbeat_registry_test.cpp` | `test_storage_heartbeat_registry` | `storage_heartbeat_registry` | 未确认到 | `storage-node`, `platform-neutral` | T045-T047、T049-T050：build 用 `test_storage_heartbeat_registry`；CTest 可直接用 `-R '^storage_heartbeat_registry$'` |
| Integrated object storage e2e | `tests/integrated_object_storage_e2e_test.cpp` | `test_integrated_object_storage_e2e` | `IntegratedObjectStorageE2ETest.*` | `integrated_object_storage_e2e` | `integrated-object-storage`, `integrated-object-storage-e2e`, `storage-transfer`, `platform-neutral`, `linux-primary-diagnosis` | T048、T051、T089-T098：build 可用 `test_integrated_object_storage_e2e` 或 `integrated_object_storage_e2e`；CTest 用 `-L integrated-object-storage-e2e` 或 `-R '^IntegratedObjectStorageE2ETest\\.'` |
| Integrated object storage quorum | `tests/integrated_object_storage_quorum_test.cpp` | `test_integrated_object_storage_quorum` | `IntegratedObjectStorageQuorumTest.*` | `integrated_object_storage_quorum` | `integrated-object-storage`, `integrated-object-storage-quorum`, `platform-neutral` | T069-T070、T078-T086、T104-T105、T114：build 可用 `test_integrated_object_storage_quorum` 或 `integrated_object_storage_quorum`；CTest 用 `-L integrated-object-storage-quorum` 或 `-R '^IntegratedObjectStorageQuorumTest\\.'` |
| Integrated object storage recovery | `tests/integrated_object_storage_recovery_test.cpp` | `test_integrated_object_storage_recovery` | `integrated_object_storage_recovery.IntegratedObjectStorageRecoveryTest.*` | `integrated_object_storage_recovery` | `integrated-object-storage`, `integrated-object-storage-recovery`, `storage-transfer`, `storage-node`, `storage-node-recovery`, `durability-boundary`, `linux-primary-diagnosis` | 009 recovery / cleanup 相关回归：build 可用 `test_integrated_object_storage_recovery` 或 `integrated_object_storage_recovery`；CTest 用 `-L integrated-object-storage-recovery` 或 `-R '^integrated_object_storage_recovery\\.'` |
| Integrated object storage concurrency | `tests/integrated_object_storage_concurrency_test.cpp` | `test_integrated_object_storage_concurrency` | `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.*` | `integrated_object_storage_concurrency` | `integrated-object-storage`, `integrated-object-storage-concurrency`, `storage-transfer`, `storage-node`, `storage-node-concurrency`, `platform-neutral`, `linux-primary-diagnosis` | 009 concurrency / stress 相关回归：build 可用 `test_integrated_object_storage_concurrency` 或 `integrated_object_storage_concurrency`；CTest 用 `-L integrated-object-storage-concurrency` 或 `-R '^integrated_object_storage_concurrency\\.'` |
| Raft election | `tests/test_raft_election.cpp` | `test_raft_election` | `RaftElectionTest.*` | 未确认到 | `platform-neutral` | T068、T073、T104：build 用 `test_raft_election`；CTest 用 `-R '^RaftElectionTest\\.'` |
| Raft replication | `tests/test_raft_log_replication.cpp` | `test_raft_log_replication` | `RaftLogReplicationTest.*` | 未确认到 | `platform-neutral` | T066、T074、T077：build 用 `test_raft_log_replication`；CTest 用 `-R '^RaftLogReplicationTest\\.'` |
| Raft snapshot catch-up | `tests/test_raft_snapshot_catchup.cpp` | `test_raft_snapshot_catchup` | `RaftSnapshotCatchupTest.*` | 未确认到 | `platform-neutral` | T067、T075、T077：build 用 `test_raft_snapshot_catchup`；CTest 用 `-R '^RaftSnapshotCatchupTest\\.'` |
| Raft restart / snapshot recovery | `tests/test_raft_snapshot_restart.cpp` | `test_raft_snapshot_restart` | `RaftSnapshotRestartTest.*`, `RaftSnapshotRecoveryTest.*` | 未确认到 | `platform-neutral`, `durability-boundary`, `linux-specific-failure-injection` | T075、T081、T083、T088：build 用 `test_raft_snapshot_restart`；CTest 用 `-R '^(RaftSnapshotRestartTest|RaftSnapshotRecoveryTest)\\.'` |
| Metadata failover | `tests/metadata_failover_test.cpp` | `test_metadata_failover` | `MetadataFailoverTest.*` | 未确认到 | `platform-neutral` | T080、T086：build 用 `test_metadata_failover`；CTest 用 `-R '^MetadataFailoverTest\\.'` |
| Metadata client scenario | `tests/metadata_client_scenario_test.cpp` | `test_metadata_client_scenario` | `MetadataClientScenarioTest.*` | 未确认到 | `platform-neutral` | 后续元数据客户端冲突/幂等回归：build 用 `test_metadata_client_scenario`；CTest 用 `-R '^MetadataClientScenarioTest\\.'` |

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
| ViewNode failover | `tests/view_node_discovery_test.cpp`, `tests/view_failover_test.cpp`, `ViewFailoverScriptValidation`, `examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view` | surviving ViewNode stays available even if peer sync continues failing; self refresh and peer sync must not overwrite live observed state; recovered ViewNode must re-converge through peer sync; `GetClusterView()` remains `OK`; discovery/status remain usable; cluster degraded or partial must not be interpreted as node unavailable |
| ViewNode registry convergence | `tests/view_failover_test.cpp`, `tests/view_node_discovery_test.cpp` | failover / recovery / peer sync can leave temporary view differences, but the final observed registry on multiple ViewNodes must converge; old incarnation snapshots must not override newer self-refresh state; stale peer snapshots must not block eventual convergence |
| ViewNode registry restart recovery boundary | `modules/view/module-notes.md`, feature `module-notes.md`, `tests/view_failover_test.cpp` | Current phase explicitly documents memory-only runtime restart recovery; restored registry snapshot merge safety and eventual reconvergence are covered by targeted tests; true runtime registry persistence remains future work and Windows/macOS restart validation stays pending |
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

## US2 Phase 5 Closure Snapshot

- Linux targeted validation:
  - build target: `test_view_node_discovery`
  - ctest regex: `ViewNodeDiscovery`
  - result: PASS (`28/28`)
  - logs:
    - `tmp/test-logs/t044-build.log`
    - `tmp/test-logs/t044-ctest.log`
- Covered US2 areas:
  - dual ViewNode registry sync
  - failover discovery
  - peer snapshot old-incarnation rejection
  - peer sync RPC export/import
  - peer sync loop regression through `ViewNodeDiscovery` coverage
- Skipped in T044:
  - Windows validation
  - macOS validation
  - local RPC multi-View smoke
  - long-running multi-View soak

## US3 Phase 6 Closure Snapshot

- Linux targeted validation:
  - build targets: `test_storage_heartbeat_registry`, `test_integrated_object_storage_e2e`
  - ctest regex: `(^storage_heartbeat_registry$|^IntegratedObjectStorageE2ETest\.)`
  - result: PASS (`storage_heartbeat_registry` + `IntegratedObjectStorageE2ETest.*` enabled cases)
  - logs:
    - `tmp/test-logs/t054-build.log`
    - `tmp/test-logs/t054-ctest.log`
- Covered US3 areas:
  - runtime StorageNode registration
  - restart with same `node_id` and new `incarnation_id`
  - duplicate `node_id` / endpoint conflict rejection
  - heartbeat runtime facts for health, writable, capacity, load, disk pressure, `incarnation_id`, and `sequence`
  - ViewNode observed-state merge feeding placement candidate discovery
  - transfer path compatibility with future dynamic placement inputs
  - committed manifest no-rebalance invariant
- Skipped in T054:
  - Windows validation
  - macOS validation
  - local RPC dynamic add-node smoke
  - long-running dynamic-join soak

## US4 Phase 7 Closure Snapshot

- Linux targeted validation:
  - build targets: `test_metadata_client_scenario`, `integrated_object_storage_quorum`, `test_view_node_discovery`
  - ctest regex:
    - `^MetadataClientScenarioTest\.`
    - `^IntegratedObjectStorageQuorumTest\.`
    - `^ViewNodeDiscoveryTest\.`
  - result: PASS
  - logs:
    - `tmp/test-logs/t065-build.log`
    - `tmp/test-logs/t065-metadata-client.log`
    - `tmp/test-logs/t065-integrated-quorum.log`
    - `tmp/test-logs/t065-view-node.log`
- Covered US4 areas:
  - dynamic Metadata candidate identity/config boundary
  - JoinMetadataCluster additive proto contract
  - Metadata leader-only join validation and follower `NOT_LEADER` + leader hint
  - metadata_node_app dynamic join candidate wiring
  - ViewNode metadata candidate discovery and `NOT_LEADER` fallback
  - AddLearner leader admission / duplicate / pending / conflict boundary
  - ViewNode observed metadata non-authoritative safety
  - committed membership / quorum unchanged by observed voter or observed joining candidate
- Skipped in T065:
  - local RPC dynamic metadata join smoke
  - learner AppendEntries catch-up
  - learner InstallSnapshot catch-up
  - promote-to-voter / odd-voter-safe promotion
  - Windows validation
  - macOS validation

## US4 Phase 8 Closure Snapshot

- Historical Linux targeted validation:
  - `test_raft_log_replication`
    - result: PASS
    - evidence:
      - `tmp/test-logs/t074-build.log`
      - `tmp/test-logs/t074-ctest.log`
  - `test_raft_snapshot_catchup`
    - result: PASS
    - evidence:
      - `tmp/test-logs/t067-build.log`
      - `tmp/test-logs/t067-ctest.log`
      - `tmp/test-logs/t075-snapshot-tests.log`
  - `integrated_object_storage_quorum`
    - result: PASS
    - evidence:
      - `tmp/test-logs/t076-build.log`
      - `tmp/test-logs/t076-ctest.log`
  - `test_raft_election`
    - result: PASS
    - evidence:
      - `tmp/test-logs/t072-ctest.log`
      - `tmp/test-logs/t072-ctest-rerun.log`
  - `metadata_client_scenario`
    - result: PASS
    - evidence:
      - `tmp/test-logs/t076-ctest.log`

- Covered US4 learner areas:
  - learner AppendEntries catch-up
  - learner InstallSnapshot catch-up
  - learner excluded from RequestVote / candidacy / leader election
  - committed-voters-only quorum calculation
  - `3 voters + 1 learner` quorum remains `2`
  - single learner promote blocked by even voter count
  - runtime voters / learners representation
  - learner log replication progress tracking
  - learner snapshot install / applied progress tracking
  - pending learner / ready-to-promote / waiting-for-pair status reporting

- Current Phase 8 semantics:
  - learner can catch up through AppendEntries and InstallSnapshot
  - learner progress is observable for diagnostics
  - learner remains non-voter before future explicit promote work
  - committed voter set remains the only authority for quorum / election / commit
  - ready learner status reporting does not modify committed membership

- T077 rerun status:
  - attempted build command:
    - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication test_raft_snapshot_catchup integrated_object_storage_quorum test_raft_election ) 9>/tmp/cqupt_raft_build.lock`
  - result: SKIPPED
  - reason: build lock not acquired

- Skipped in Phase 8 closure:
  - Windows validation
  - macOS validation
  - local RPC runtime dynamic metadata learner join smoke
  - promote-to-voter
  - batch promote
  - joint consensus

## US4 Phase 9 Batch Promote Closure Snapshot

- Linux targeted validation:
  - build targets:
    - `integrated_object_storage_quorum`
    - `test_metadata_failover`
    - `test_raft_snapshot_restart`
  - ctest regex:
    - `IntegratedObjectStorageQuorum|MetadataFailover|RaftSnapshotRestart|RaftSnapshotRecovery|SnapshotRestart`
  - result: PASS (`36/36`)
  - logs:
    - `tmp/test-logs/t086-build.log`
    - `tmp/test-logs/t086-ctest.log`

- Covered US4 batch promote areas:
  - `3 voters + 2 ready learners -> committed 5 voters`
  - quorum stays committed-voters-only before promote
  - single learner promote blocked by even voter count
  - no committed `4-voter` history
  - leader failure during batch promote does not leave partial committed membership
  - snapshot / restart recovery restores committed `5-voter` membership with quorum `3`
  - ViewNode observation remains non-authoritative for membership change

- Covered key CTest entries:
  - `IntegratedObjectStorageQuorumTest.ThreeVotersPlusObservedLearnerKeepsCommittedQuorumAtTwo`
  - `IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`
  - `IntegratedObjectStorageQuorumTest.SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal`
  - `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
  - `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
  - `MetadataFailoverTest.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership`
  - `RaftSnapshotRestartTest.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership`

- Skipped in Phase 9 closure:
  - Windows validation
  - macOS validation
  - local RPC dynamic metadata join + batch promote smoke
  - long-running failover / soak
  - multi-ViewNode discovery interaction with promote target selection runtime smoke
  - joint consensus implementation / protocol-level validation

## US6 Phase 10 Local RPC Example Log Path Snapshot

- Linux minimal smoke:
  - script entry:
    - `examples/object-storage-local-009-dynamic/qidong.sh`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/tingzhi.sh`
  - result: PASS
  - logs:
    - `tmp/test-logs/t096-check-ignore.log`
    - `tmp/test-logs/t096-git-status-ignored.log`
    - `tmp/test-logs/t096-status.log`
    - `tmp/test-logs/t096-cleanup.log`
    - `tmp/test-logs/t096-summary.log`

- Confirmed local ignored paths:
  - process logs: `examples/object-storage-local-009-dynamic/logs/`
  - pid files: `examples/object-storage-local-009-dynamic/pids/`
  - validation command outputs: `tmp/test-logs/`

- Covered Phase 10 log/output areas:
  - `view-1` / `view-2` / `meta-1..3` / `store-1..6` process logs are split into dedicated files
  - dynamic `store-7` / `meta-4` / `meta-5` keep the same per-node log naming convention when started
  - failover temporary client config stays under local ignored path:
    - `examples/object-storage-local-009-dynamic/logs/failover-view-2-client.json`
  - command outputs for startup / shutdown / status / join / promote / failover remain traceable through `tmp/test-logs/t09x-*.log`
  - task reports can summarize these outputs without embedding full node logs

- Current boundary:
  - runtime `data_dir` / identity / snapshot / chunk data remain local runtime artifacts and are not copied into task reports
  - no production code changes were required for T096

- Skipped in T096:
  - Windows validation
  - macOS validation
  - extended roundtrip / failover rerun beyond minimal smoke

## US6 Phase 10 Local RPC Dynamic Validation Snapshot

- Linux full dynamic example:
  - script entry:
    - `examples/object-storage-local-009-dynamic/qidong.sh`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh join-storage`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh promote-metadata-learner`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner-2`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh promote-metadata-learners`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh status`
    - `examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view`
  - result: FAIL
  - logs:
    - `tmp/test-logs/t098-startup.log`
    - `tmp/test-logs/t098-status-01.log`
    - `tmp/test-logs/t098-roundtrip-01.log`
    - `tmp/test-logs/t098-join-storage.log`
    - `tmp/test-logs/t098-status-02.log`
    - `tmp/test-logs/t098-roundtrip-02.log`
    - `tmp/test-logs/t098-join-metadata-learner.log`
    - `tmp/test-logs/t098-status-03.log`
    - `tmp/test-logs/t098-promote-metadata-learner.log`
    - `tmp/test-logs/t098-status-04.log`
    - `tmp/test-logs/t098-join-metadata-learner-2.log`
    - `tmp/test-logs/t098-status-05.log`
    - `tmp/test-logs/t098-promote-metadata-learners.log`
    - `tmp/test-logs/t098-status-06.log`
    - `tmp/test-logs/t098-failover-view.log`
    - `tmp/test-logs/t098-cleanup.log`

- Passed Phase 10 runtime areas:
  - `2 ViewNodes + 3 Metadata voters + 6 StorageNodes` startup
  - initial `status`
  - initial real `roundtrip`
  - runtime `store-7` join and `storage_nodes=7` observation
  - post-join `status`
  - post-join real `roundtrip`
  - runtime `meta-4` learner join
  - single learner blocked promote with committed voters `3` and quorum `2`
  - runtime `meta-5` learner join
  - batch promote observation with committed voters `5`, quorum `3`, and no observed committed `4-voter` state

- Failed Phase 10 runtime area:
  - `rpc_demo.sh failover-view`
    - failure summary:
      - `surviving_view_status_unavailable`
      - `wait_seconds=30`
    - consequence:
      - failover 之后的独立 `status` / `roundtrip` 步骤未进入 PASS 结论

- Current boundary:
  - batch promote runtime path is now observed in the local RPC example
  - ViewNode failover runtime path is not yet stable enough to close Phase 10

- Pending in T098:
  - Windows validation
  - macOS validation
  - failover-after-batch-promote runtime stabilization

## T099-T101 ViewNode Failover Stabilization Closure Snapshot

- Historical context:
  - T098 captured a real local RPC failover-view failure with `reason=surviving_view_status_unavailable`
  - T099 confirmed the false negative came from over-strict script readiness gates rather than `modules/view/*` propagating `peer sync failure -> self unavailable`
  - T100 added dedicated regression coverage so this boundary is no longer protected only by script assertions

- Linux validation evidence reused by T101:
  - T099:
    - targeted build: PASS
    - targeted CTest `ViewFailover|FailoverView|ViewNode`: PASS
    - `ViewFailoverScriptValidation`: PASS
    - local RPC `rpc_demo.sh failover-view`: PASS
    - logs:
      - `tmp/test-logs/t099-build.log`
      - `tmp/test-logs/t099-ctest.log`
      - `tmp/test-logs/t099-view-failover-script.log`
      - `tmp/test-logs/t099-failover-view.log`
  - T100:
    - build target: `view_failover_test`
    - targeted CTest `ViewFailover|FailoverView|ViewNode`: PASS (`37/37`)
    - logs:
      - `tmp/test-logs/t100-view-failover-build.log`
      - `tmp/test-logs/t100-view-failover-ctest.log`

- Covered stabilization semantics:
  - `peer sync` connection-refused / backoff diagnostics remain visible
  - `peer sync failure` does not imply self unavailable
  - surviving ViewNode remains `liveness=live`
  - surviving ViewNode may be `healthy` or `degraded`, but not misclassified as `unavailable`
  - `GetClusterView()` remains `kOk`
  - `DiscoverMetadata()` remains available through the surviving ViewNode
  - partial storage registry may legitimately yield `kNotFound`
  - `cluster degraded` / `partial` must not propagate into `node unavailable`
  - ViewNode remains observation/discovery only and is not membership authority

- Still not covered by this closure:
  - Windows runtime validation
  - macOS runtime validation
  - long-running peer sync disconnect/retry soak
  - restart-after-failover registry rehydration beyond the current memory-only boundary

## T102 Multi-View Self-Refresh / Peer-Sync / Failover Closure Snapshot

- Linux targeted validation:
  - build targets:
    - `view_failover_test`
    - `test_view_node_discovery`
  - ctest regex:
    - `ViewFailover|ViewNode`
  - result: PASS (`39/39`)
  - logs:
    - `tmp/test-logs/t102-view-build.log`
    - `tmp/test-logs/t102-view-ctest.log`

- New regression coverage added in T102:
  - `ViewFailoverTest.MultiViewSelfRefreshAndPeerSyncPreserveAvailabilityAcrossFailover`
  - `ViewFailoverTest.RecoveredViewNodePeerSyncReconvergesWithoutOverwritingLiveState`

- Covered multi-View semantics:
  - self refresh state is not overwritten by stale peer-synced view records
  - peer sync does not propagate `unavailable` to a live surviving ViewNode
  - failover after peer sync still keeps surviving ViewNode available
  - registry metadata/storage facts continue to synchronize through peer snapshots
  - recovered ViewNode re-joins and converges back to `live` through peer sync
  - `GetClusterView()`, `DiscoverMetadata()`, and `DiscoverStorage()` remain usable during failover and after re-convergence

- Still not covered by T102:
  - Windows runtime validation
  - macOS runtime validation
  - long-running multi-View soak / repeated disconnect-retry cycles

## T103 ViewNode Registry Convergence Closure Snapshot

- Linux targeted validation:
  - build targets:
    - `view_failover_test`
    - `test_view_node_discovery`
  - ctest regex:
    - `ViewNode|ViewFailover`
  - result: PASS (`40/40`)
  - logs:
    - `tmp/test-logs/t103-view-build.log`
    - `tmp/test-logs/t103-view-ctest.log`

- New regression coverage added in T103:
  - `ViewFailoverTest.RegistryConvergesAcrossViewNodesAfterFailoverRecoveryAndPeerSync`

- Covered convergence semantics:
  - failover after peer sync may create temporary view differences, but final cluster view converges on both ViewNodes
  - recovered ViewNode re-joins, self-refreshes, and converges back through bidirectional peer sync
  - stale pre-failover snapshot import produces `stale_ignored` behavior and does not overwrite newer incarnation-aware state
  - metadata/storage observed facts converge to the same final values on both ViewNodes
  - final view-node, metadata-node, and storage-node counts match across peers

- Still not covered by T103:
  - Windows runtime validation
  - macOS runtime validation
  - long-running repeated peer disconnect / retry soak
  - restart persistence beyond the documented memory-only registry recovery boundary
