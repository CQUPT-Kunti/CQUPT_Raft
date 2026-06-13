# Tasks: 009 Local RPC Object Storage Stabilization

**Input**: Design documents from `specs/009-local-rpc-object-storage-stabilization/`  
**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`, `validation-matrix.md`  
**Tests**: 用户明确要求测试和端到端验证；高风险路径按测试优先组织。  
**Organization**: 按用户要求的 12 个阶段组织，并映射到 `spec.md` 的 user stories。

## Format: `[ID] [P?] [Story] Description`

- `[P]`: 可并行，前提是写入文件不冲突且依赖阶段已完成。
- `[US1]`: ViewNode self refresh。
- `[US2]`: 多 ViewNode active-active discovery。
- `[US3]`: StorageNode dynamic join。
- `[US4]`: Metadata learner join / odd voter。
- `[US5]`: identity lifecycle。
- `[US6]`: local RPC example / stability validation。

---

## Phase 1: Existing Local RPC / Test / Report Survey And Spec Landing

**Purpose**: 锁定 008 真实入口和 009 文档边界，避免后续任务绕开现有路径。

- [X] T001 Record report-derived example, scripts, app targets, CTest targets, and known gaps in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-01-survey.md` (验证: `specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`)
- [X] T002 [P] Confirm `examples/object-storage-local-3meta-6store/qidong.sh`, `examples/object-storage-local-3meta-6store/tingzhi.sh`, and `examples/object-storage-local-3meta-6store/rpc_demo.sh` remain the local RPC baseline in `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md` (验证: local RPC scripts)
- [X] T003 [P] Confirm CTest target and label coverage from `tests/CMakeLists.txt` in `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md` (验证: `test_view_node_discovery`, `test_node_identity`, `storage_heartbeat_registry`, `integrated_object_storage_e2e`, `integrated_object_storage_quorum`)
- [X] T004 [P] Confirm existing identity, ViewNode, StorageNode, Metadata, Raft membership entry points in `specs/009-local-rpc-object-storage-stabilization/module-notes.md` (验证: `tests/node_identity_test.cpp`, `tests/view_node_discovery_test.cpp`, `tests/storage_heartbeat_registry_test.cpp`, `tests/integrated_object_storage_quorum_test.cpp`)
- [X] T005 Add phase report template for later implementation results in `specs/009-local-rpc-object-storage-stabilization/task-reports/task-report-template.md` (验证: task report path exists and references project log rules)

---

## Phase 2: identity_file / Node Identity Lifecycle Closure

**Goal**: `identity_file` 首次缺失时创建身份，重启复用 `node_id`，并区分长期身份、process incarnation、sequence、observed_time。

**Independent Test**: `tests/node_identity_test.cpp` 覆盖 Storage/View/Metadata bootstrap/Metadata dynamic join 的首次创建、重启复用、mismatch、corrupt file。

- [X] T006 [P] [US5] Add StorageNode first-start identity creation tests in `tests/node_identity_test.cpp` (验证: `test_node_identity`)
- [X] T007 [P] [US5] Add ViewNode first-start identity creation and restart reuse tests in `tests/node_identity_test.cpp` (验证: `test_node_identity`)
- [X] T008 [P] [US5] Add Metadata bootstrap voter identity tests with fixed `node_id` and `raft_id` in `tests/node_identity_test.cpp` (验证: `test_node_identity`, `cluster_config_test`)
- [X] T009 [P] [US5] Add Metadata dynamic join candidate identity tests in `tests/node_identity_test.cpp` (验证: `test_node_identity`)
- [X] T010 [P] [US5] Add mismatch and corrupt identity fail-fast tests in `tests/node_identity_test.cpp` (验证: `test_node_identity`)
- [X] T011 [US5] Extend identity data model for node type, optional `raft_id`, membership state, and persistent generation in `modules/cluster/node_identity.h` and `modules/cluster/node_identity.cpp` (验证: `test_node_identity`)
- [X] T012 [US5] Add atomic first-start identity creation and restart validation in `modules/cluster/node_identity.cpp` (验证: `test_node_identity`)
- [X] T013 [US5] Add process incarnation / boot epoch generation boundary in `modules/cluster/node_identity.cpp` or a new `modules/cluster` helper (验证: `test_node_identity`)
- [X] T014 [US5] Wire StorageNode identity load/create into `apps/storage_node_app.cpp` without requiring full topology config (验证: `storage_heartbeat_registry`, local RPC startup script)
- [X] T015 [US5] Wire ViewNode identity load/create into `apps/view_node_app.cpp` before self registration (验证: `test_view_node_discovery`)
- [X] T016 [US5] Wire Metadata bootstrap vs dynamic join identity modes into `apps/metadata_node_app.cpp` (验证: `cluster_config_test`, `test_node_identity`)
- [X] T017 [US5] Document platform durability behavior for identity atomic publish in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-02-identity.md` (验证: `test_node_identity`, Windows/macOS marked pending if not run)

---

## Phase 3: ViewNode Self State Refresh And Liveness Fix

**Goal**: 健康运行中的 ViewNode 不因缺少外部 heartbeat 而把自己判为 stale/suspect/dead。

**Independent Test**: 单 ViewNode 运行超过 dead TTL 后自身仍为 `LIVE`；停止 self refresh 后 TTL 转换正常。

- [X] T018 [P] [US1] Add deterministic self refresh beyond TTL test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [X] T019 [P] [US1] Add self refresh disabled stale/suspect/dead transition test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [X] T020 [P] [US1] Add ViewNode self liveness regression to local RPC status expectations in `examples/object-storage-local-3meta-6store/rpc_demo.sh` or a new 009 example script (验证: `rpc_demo.sh status`)
- [X] T021 [US1] Add ViewNode self refresh state update path in `modules/view/view_registry.cpp` and `modules/view/view_registry.h` (验证: `test_view_node_discovery`)
- [X] T022 [US1] Start and stop ViewNode self refresh loop from `apps/view_node_app.cpp` with clean shutdown semantics (验证: `test_view_node_discovery`, local RPC startup/shutdown scripts)
- [X] T023 [US1] Ensure self refresh payload includes node_id, endpoint, incarnation, sequence, observed_time, health, and liveness in `modules/view/view_registry.cpp` (验证: `test_view_node_discovery`)
- [X] T024 [US1] Add diagnostics for ViewNode self refresh sequence and liveness in `modules/view/view_service_impl.cpp` (验证: `test_view_node_discovery`, `rpc_demo.sh status`)
- [X] T025 [US1] Record Linux validation and skipped platform checks in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-03-view-self-refresh.md` (验证: `test_view_node_discovery`)

---

## Phase 4: ViewNode Incarnation / Sequence / Registry Merge Protection

**Goal**: 旧 process incarnation、旧 heartbeat、旧 registry snapshot 不得覆盖新状态。

**Independent Test**: 更高 incarnation 优先；同 incarnation 更高 sequence 优先；`observed_time` 不单独作为覆盖依据。

- [X] T026 [P] [US1] Add higher incarnation wins test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [X] T027 [P] [US1] Add same-incarnation higher sequence wins test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [X] T028 [P] [US1] Add observed_time-only stale override rejection test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [X] T029 [P] [US5] Add identity restart old-incarnation rejection test in `tests/node_identity_test.cpp` (验证: `test_node_identity`)
- [X] T030 [US1] Add incarnation-aware observed state fields to `modules/view/view_registry.h` and implementation in `modules/view/view_registry.cpp` (验证: `test_view_node_discovery`)
- [X] T031 [US1] Implement deterministic merge ordering in `modules/view/view_registry.cpp` (验证: `test_view_node_discovery`)
- [X] T032 [US1] Add conflict diagnostics for duplicate node_id, endpoint, and data_dir fingerprint in `modules/view/view_registry.cpp` (验证: `test_view_node_discovery`)
- [X] T033 [US1] Update ViewNode RPC/protobuf adapter mapping for incarnation and sequence in `modules/view/view_service_impl.cpp` and `modules/view/view_client.cpp` (验证: `test_view_node_discovery`)
- [X] T034 [US1] Record merge behavior and stale snapshot limitations in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-04-view-merge.md` (验证: `test_view_node_discovery`)

---

## Phase 5: Multi-ViewNode Peer Registry Sync

**Goal**: 至少两个 ViewNode active-active discovery，任一故障时仍能通过另一个发现 metadata/storage。

**Independent Test**: 向一个 ViewNode 注册状态，另一个 ViewNode 通过 peer sync 可见；停一个 ViewNode 后 survivor 仍能 discovery。

- [x] T035 [P] [US2] Add dual ViewNode registry sync test in `tests/view_node_discovery_test.cpp` or a new `tests/view_node_peer_sync_test.cpp` (验证: `test_view_node_discovery`)
- [x] T036 [P] [US2] Add ViewNode failover discovery test in `tests/view_node_discovery_test.cpp` or integration test (验证: `test_view_node_discovery`, local RPC status)
- [X] T037 [P] [US2] Add peer snapshot old-incarnation rejection test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [x] T038 [US2] Add peer ViewNode seed configuration parsing in `modules/cluster/cluster_config.cpp` and `modules/cluster/cluster_config.h` (验证: `cluster_config_test`)
- [x] T039 [US2] Add ViewNode peer sync client/server contract in `proto/view.proto` and adapters in `modules/view/view_service_impl.cpp` and `modules/view/view_client.cpp` (验证: `test_view_node_discovery`)
- [X] T040 [US2] Implement Push/Pull/Merge observed registry sync in `modules/view/view_registry.cpp` (验证: `test_view_node_discovery`)
- [x] T041 [US2] Start peer sync loop and retry/backoff from `apps/view_node_app.cpp` (验证: `test_view_node_discovery`, local RPC startup/shutdown scripts)
- [x] T042 [US2] Define ViewNode registry persistence or memory-only restart recovery boundary in `modules/view/module-notes.md` and feature `module-notes.md` (验证: `test_view_node_discovery`)
- [X] T043 [US2] Add CMake wiring for any new ViewNode peer sync test in `tests/CMakeLists.txt` (验证: CTest label `view-node`)
- [x] T044 [US2] Record dual ViewNode validation in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-05-view-peer-sync.md` (验证: `test_view_node_discovery`, local RPC status)

---

## Phase 6: StorageNode Dynamic Registration And Placement Visibility

**Goal**: 运行中新增 StorageNode，注册到 ViewNode，后续新对象 placement 可使用它，不影响 Raft quorum。

**Independent Test**: 集群运行中启动新 StorageNode，discovery 可见，后续 upload/download 可使用该节点。

- [x] T045 [P] [US3] Add run-time StorageNode registration test in `tests/storage_heartbeat_registry_test.cpp` (验证: `storage_heartbeat_registry`)
- [x] T046 [P] [US3] Add StorageNode restart same node_id new incarnation test in `tests/storage_heartbeat_registry_test.cpp` (验证: `storage_heartbeat_registry`)
- [X] T047 [P] [US3] Add duplicate node_id/endpoint conflict test in `tests/storage_heartbeat_registry_test.cpp` (验证: `storage_heartbeat_registry`)
- [x] T048 [P] [US3] Add dynamic StorageNode placement integration test in `tests/integrated_object_storage_e2e_test.cpp` (验证: `integrated_object_storage_e2e`)
- [x] T049 [US3] Extend StorageNode heartbeat payload with capacity, load, disk pressure, health, writable status, incarnation, and sequence in `modules/store/node/storage_node_service.cpp` and `modules/store/node/storage_node_client.cpp` (验证: `storage_heartbeat_registry`)
- [x] T050 [US3] Make StorageNode app register/heartbeat to ViewNode seed list or first available ViewNode in `apps/storage_node_app.cpp` (验证: `storage_heartbeat_registry`, local RPC startup)
- [x] T051 [US3] Ensure ViewNode storage observed state merge feeds placement candidate discovery in `modules/view/view_registry.cpp` and `modules/store/placement/placement_manager.cpp` (验证: `integrated_object_storage_e2e`)
- [x] T052 [US3] Preserve existing transfer path in `modules/store/transfer/object_transfer.cpp` while allowing future placement to include dynamic StorageNode (验证: `integrated_object_storage_e2e`)
- [x] T053 [US3] Add no-rebalance invariant diagnostics for committed object manifest in `modules/store/transfer/metadata_transfer_client.cpp` or metadata integration path (验证: `integrated_object_storage_e2e`)
- [x] T054 [US3] Record dynamic StorageNode Linux validation in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-06-storage-dynamic-join.md` (验证: `storage_heartbeat_registry`, `integrated_object_storage_e2e`)

---

## Phase 7: MetadataNode Dynamic Learner Join Interface And Safety Constraints

**Goal**: 新 MetadataNode 先注册为 observed metadata node，再通过 Metadata leader 申请 learner join；ViewNode 不越权修改 Raft membership。

**Independent Test**: Dynamic MetadataNode candidate 通过 ViewNode 发现 leader，JoinMetadataCluster 只产生 learner/non-voter membership change。

- [x] T055 [P] [US4] Add dynamic Metadata candidate identity/config tests in `tests/cluster_config_test.cpp` and `tests/node_identity_test.cpp` (验证: `cluster_config_test`, `test_node_identity`)
- [x] T056 [P] [US4] Add JoinMetadataCluster leader validation tests in `tests/metadata_client_scenario_test.cpp` (验证: `metadata_client_scenario_test`)
- [X] T057 [P] [US4] Add duplicate join and pending membership change rejection tests in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [x] T058 [P] [US4] Add ViewNode-observed metadata registration test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [x] T059 [US4] Add additive JoinMetadataCluster request/response contract in `proto/raft.proto` or `proto/metadata.proto` (验证: generated proto build, `metadata_client_scenario_test`)
- [x] T060 [US4] Implement Metadata leader join validation in `modules/raft/service/metadata_service_impl.cpp` (验证: `metadata_client_scenario_test`, `integrated_object_storage_quorum`)
- [x] T061 [US4] Add dynamic join mode wiring in `apps/metadata_node_app.cpp` without changing initial bootstrap voter startup (验证: `cluster_config_test`, local RPC startup)
- [x] T062 [US4] Add leader discovery through ViewNode candidates and `NOT_LEADER` fallback in `apps/metadata_node_app.cpp` or metadata client helper (验证: `metadata_client_scenario_test`)
- [x] T063 [US4] Add AddLearner proposal path stub/implementation boundary in `modules/raft/node/raft_node.cpp` and `modules/raft/node/raft_node.h` (验证: `integrated_object_storage_quorum`)
- [x] T064 [US4] Ensure ViewNode metadata observations remain non-authoritative in `modules/view/view_registry.cpp` and `modules/raft/service/metadata_service_impl.cpp` (验证: `test_view_node_discovery`, `integrated_object_storage_quorum`)
- [x] T065 [US4] Record join API and safety validation in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-07-metadata-join.md` (验证: `metadata_client_scenario_test`, `integrated_object_storage_quorum`)

---

## Phase 8: Learner Catch-Up / Pending Learner / Odd Voter Constraint

**Goal**: learner 可以追日志和 snapshot，但未 promote 前不参与 quorum/election；单 learner ready 不得让 voter count 变偶数。

**Independent Test**: 3 voters + 1 learner quorum 仍为 2；learner 可接收 AppendEntries/InstallSnapshot；单独 promote 被明确阻止。

- [x] T066 [P] [US4] Add learner AppendEntries catch-up test in `tests/test_raft_log_replication.cpp` (验证: `test_raft_log_replication`)
- [x] T067 [P] [US4] Add learner InstallSnapshot catch-up test in `tests/test_raft_snapshot_catchup.cpp` (验证: `test_raft_snapshot_catchup`)
- [x] T068 [P] [US4] Add learner excluded from RequestVote and leader election test in `tests/test_raft_election.cpp` (验证: `test_raft_election`)
- [x] T069 [P] [US4] Add 3 voters + 1 learner quorum remains 2 test in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [x] T070 [P] [US4] Add single learner promote blocked by even voter count test in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [x] T071 [US4] Extend runtime membership representation for voters and learners in `modules/raft/node/raft_node.h` and `modules/raft/node/raft_node.cpp` (验证: `test_raft_election`, `integrated_object_storage_quorum`)
- [x] T072 [US4] Update quorum calculation to use committed voters only in `modules/raft/node/raft_node.cpp` (验证: `integrated_object_storage_quorum`)
- [x] T073 [US4] Exclude learners from RequestVote and candidacy paths in `modules/raft/node/raft_node.cpp` (验证: `test_raft_election`)
- [x] T074 [US4] Enable learner log replication progress tracking in `modules/raft/replication/replicator.cpp` and `modules/raft/replication/replicator.h` (验证: `test_raft_log_replication`)
- [x] T075 [US4] Enable learner snapshot install and applied progress tracking in `modules/raft/replication/replicator.cpp` and snapshot integration paths (验证: `test_raft_snapshot_catchup`, `test_raft_snapshot_restart`)
- [x] T076 [US4] Add pending learner / ready-to-promote / waiting-for-pair status reporting in `modules/raft/service/metadata_service_impl.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T077 [US4] Record learner catch-up validation in `specs/009-local-rpc-object-storage-stabilization/task-reports/t077-record-learner-catch-up-validation.md` (验证: `test_raft_log_replication`, `test_raft_snapshot_catchup`, `integrated_object_storage_quorum`)

---

## Phase 9: Batch Promote / Joint Consensus / Batched Membership Change

**Goal**: 支持 3 voters + 2 ready learners 安全扩到 5 voters，不能提交 4-voter 中间配置。

**Independent Test**: 两个 learners ready 后 committed voters 直接变为 5，quorum 从 2 变 3，中间没有 committed 4 voters。

- [ ] T078 [P] [US4] Add 3 voters + 2 ready learners batch promote test in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T079 [P] [US4] Add no committed 4-voter history assertion in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T080 [P] [US4] Add leader failure during batch promote test in `tests/metadata_failover_test.cpp` (验证: `metadata_failover_test`)
- [ ] T081 [P] [US4] Add restart recovery for committed batch membership in `tests/test_raft_snapshot_restart.cpp` (验证: `test_raft_snapshot_restart`)
- [ ] T082 [US4] Design and implement batched membership change or joint consensus state in `modules/raft/node/raft_node.cpp` and `modules/raft/node/raft_node.h` (验证: `integrated_object_storage_quorum`)
- [ ] T083 [US4] Persist and apply batch membership changes through Raft log/config entry path in `modules/raft/storage` and `modules/raft/node/raft_node.cpp` (验证: `test_raft_snapshot_restart`, `integrated_object_storage_quorum`)
- [ ] T084 [US4] Add target voter odd-count validation before membership proposal commit in `modules/raft/node/raft_node.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T085 [US4] Promote two ready learners together and update quorum summary in `modules/raft/service/metadata_service_impl.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T086 [US4] Ensure batch promote handles concurrent or duplicate pending changes in `modules/raft/node/raft_node.cpp` (验证: `metadata_failover_test`, `integrated_object_storage_quorum`)
- [ ] T087 [US4] If safe batch promote cannot be finished in this milestone, document learner-only completion and blocked promote status in `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md` (验证: `integrated_object_storage_quorum`)
- [ ] T088 [US4] Record batch promote validation in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-09-batch-promote.md` (验证: `integrated_object_storage_quorum`, `metadata_failover_test`, `test_raft_snapshot_restart`)

---

## Phase 10: Local RPC Example Extension And End-To-End Validation

**Goal**: 扩展 local RPC example 覆盖 2 ViewNode、3 initial metadata voters、多 StorageNodes、运行中 StorageNode join、1/2 Metadata learners、ViewNode failover。

**Independent Test**: 用 example 脚本完成动态加入和真实 upload/download，不是静态配置后一键启动。

- [ ] T089 [P] [US6] Add or extend 009 local RPC topology config under `examples/object-storage-local-3meta-6store/cluster.json` or a sibling 009 example config (验证: app targets build, local RPC startup)
- [ ] T090 [P] [US6] Add 2-ViewNode startup support in `examples/object-storage-local-3meta-6store/qidong.sh` or a sibling 009 startup script (验证: local RPC startup)
- [ ] T091 [P] [US6] Add matching shutdown support in `examples/object-storage-local-3meta-6store/tingzhi.sh` or a sibling 009 shutdown script (验证: local RPC shutdown)
- [ ] T092 [US6] Add run-time StorageNode join command to `examples/object-storage-local-3meta-6store/rpc_demo.sh` or a sibling 009 script (验证: local RPC dynamic storage join)
- [ ] T093 [US6] Add run-time Metadata learner join command for one learner and blocked promote observation in example scripts (验证: local RPC metadata join)
- [ ] T094 [US6] Add second learner join and batch promote observation in example scripts (验证: local RPC metadata join, quorum status)
- [ ] T095 [US6] Add ViewNode failover status and roundtrip path to example scripts (验证: local RPC status, roundtrip)
- [ ] T096 [US6] Ensure example logs are written under an ignored/local log path and summarized in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-10-local-rpc-example.md` (验证: project test-log rules)
- [ ] T097 [US6] Run targeted app build for `view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client` before local RPC validation (验证: targeted CMake build)
- [ ] T098 [US6] Record local RPC dynamic validation results in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-10-local-rpc-example.md` (验证: `qidong.sh`, `rpc_demo.sh status`, `rpc_demo.sh roundtrip`, `tingzhi.sh`)

---

## Phase 11: Failure / Restart / Duplicate / TTL / Old Incarnation / Odd Voter Tests

**Goal**: 补齐稳定性矩阵，不停留在 happy path。

**Independent Test**: validation matrix 中的每个故障/重启/重复/TTL/odd-voter 场景都有 CTest 或 local RPC example 入口。

- [ ] T099 [P] [US6] Add ViewNode restart and peer old snapshot protection test in `tests/view_node_discovery_test.cpp` (验证: `test_view_node_discovery`)
- [ ] T100 [P] [US6] Add StorageNode duplicate registration and stale heartbeat test in `tests/storage_heartbeat_registry_test.cpp` (验证: `storage_heartbeat_registry`)
- [ ] T101 [P] [US6] Add identity damaged-file, type mismatch, and cluster mismatch coverage in `tests/node_identity_test.cpp` (验证: `test_node_identity`)
- [ ] T102 [P] [US6] Add Metadata duplicate join request and idempotency/conflict test in `tests/metadata_client_scenario_test.cpp` (验证: `metadata_client_scenario_test`)
- [ ] T103 [P] [US6] Add leader change during learner catch-up scenario in `tests/metadata_failover_test.cpp` (验证: `metadata_failover_test`)
- [ ] T104 [P] [US6] Add learner not counted in quorum and not eligible for election cross-check in `tests/test_raft_election.cpp` and `tests/integrated_object_storage_quorum_test.cpp` (验证: `test_raft_election`, `integrated_object_storage_quorum`)
- [ ] T105 [P] [US6] Add committed membership odd-count invariant history checks in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T106 [US6] Update `tests/CMakeLists.txt` with any new 009 test binaries and labels without removing existing tests (验证: targeted CTest labels)
- [ ] T107 [US6] Update `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md` with actual test names and skipped platform items (验证: validation matrix complete)
- [ ] T108 [US6] Record stability matrix validation in `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-11-stability-matrix.md` (验证: relevant CTest names and local RPC scripts)

---

## Phase 12: Documentation, Reports, Acceptance Matrix Closure

**Purpose**: 收口 009 交付状态、风险、验证矩阵和模块说明。

- [ ] T109 [P] Update `specs/009-local-rpc-object-storage-stabilization/module-notes.md` with final responsibilities, state transitions, and misuse warnings (验证: module-notes reviewed against touched modules)
- [ ] T110 [P] Update `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md` with resolved and residual risks (验证: risk notes reviewed against failed/skipped tests)
- [ ] T111 [P] Update `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md` with PASS/SKIP/PENDING status and log paths (验证: task reports)
- [ ] T112 Add final task summary in `specs/009-local-rpc-object-storage-stabilization/task-reports/final-summary.md` (验证: all phase task reports referenced)
- [ ] T113 Confirm `specs/009-local-rpc-object-storage-stabilization/spec.md`, `plan.md`, and `tasks.md` do not contain execution logs (验证: document review)
- [ ] T114 Confirm no committed voter membership test allows even voter count in `tests/integrated_object_storage_quorum_test.cpp` (验证: `integrated_object_storage_quorum`)
- [ ] T115 Run final targeted Linux validation set from `specs/009-local-rpc-object-storage-stabilization/quickstart.md` when build lock is available (验证: targeted CMake/CTest/local RPC commands)
- [ ] T116 Record Windows/macOS pending or smoke results in `specs/009-local-rpc-object-storage-stabilization/task-reports/final-summary.md` (验证: explicit pending status if no machines)

---

## Dependencies & Execution Order

- Phase 1 blocks all implementation phases because it freezes real entry points.
- Phase 2 blocks Phase 3-10 because incarnation/identity semantics are shared.
- Phase 3 and Phase 4 block Phase 5 because peer sync merge safety depends on self state and incarnation ordering.
- Phase 6 can start after Phase 2 and ViewNode registry merge basics are available.
- Phase 7 starts after Phase 2 and requires Metadata leader discovery through ViewNode.
- Phase 8 depends on Phase 7 AddLearner membership entry.
- Phase 9 depends on Phase 8 learner catch-up and ready-to-promote states.
- Phase 10 depends on Phases 3, 5, 6, 8, and 9 for full scenario coverage; it can first validate partial scenarios when Phase 9 is still blocked.
- Phase 11 runs after the relevant feature phases but individual tests can be added earlier.
- Phase 12 closes after desired validation reports exist.

## Parallel Opportunities

- T002-T004 can run in parallel.
- T006-T010 can run in parallel as identity tests before implementation.
- T018-T020 can run in parallel as ViewNode self refresh tests.
- T026-T029 can run in parallel as merge/identity regression tests.
- T035-T037 can run in parallel as peer sync tests.
- T045-T048 can run in parallel as StorageNode dynamic join tests.
- T055-T058 can run in parallel as Metadata join tests.
- T066-T070 can run in parallel as learner/quorum/election tests.
- T078-T081 can run in parallel as batch promote/failover/restart tests.
- T099-T105 can run in parallel after core behavior exists.

## Implementation Strategy

1. Preserve the 008 static local RPC roundtrip first.
2. Close identity/incarnation semantics before registry merge or dynamic join.
3. Deliver ViewNode self refresh and merge safety before multi-ViewNode sync.
4. Deliver StorageNode dynamic join as discovery-only without touching Raft quorum.
5. Deliver Metadata dynamic join as Raft learner membership only; ViewNode remains observation-only.
6. Enforce odd committed voter count before any promote path.
7. Implement batch promote / joint consensus only after learner catch-up and blocked single-promote tests are passing.
8. Extend local RPC example last, using CTest and unit/integration tests to de-risk the runtime scenario first.
