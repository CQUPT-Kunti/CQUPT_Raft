# Tasks: Integrated Object Storage System

**Input**: Design documents from `/specs/008-integrated-object-storage-system/`  
**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/](contracts/), [quickstart.md](quickstart.md)

**Tests**: 本阶段明确要求端到端真实文件、quorum、StorageNode 故障/重启、并发上传下载、checksum mismatch、未 commit 数据清理和跨平台启动验证，因此每个高风险 user story 都包含测试任务。

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- CQUPT_Raft source modules live under `modules/`
- Raft control-plane modules live under `modules/raft/`
- StorageNode data-plane modules live under `modules/store/`
- Planned cluster config/identity helpers live under `modules/cluster/`
- Planned ViewNode discovery module lives under `modules/view/`
- Protocol definitions live under `proto/`
- Entrypoints live under `apps/`
- Tests live under `tests/`
- Spec artifacts live under `specs/008-integrated-object-storage-system/`

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: 建立本阶段共享构建、契约和测试入口，不实现业务逻辑。

- [x] T001 Confirm affected module notes and append 008 ownership boundaries in `modules/store/upload/module-notes.md`, `modules/store/placement/module-notes.md`, `modules/store/node/module-notes.md`
- [X] T002 [P] Create `modules/cluster/AGENTS.md` and `modules/cluster/module-notes.md` documenting config/identity ownership and durability boundaries
- [x] T003 [P] Create `modules/view/AGENTS.md` and `modules/view/module-notes.md` documenting ViewNode discovery-only authority boundaries
- [X] T004 [P] Create `modules/store/transfer/AGENTS.md` and `modules/store/transfer/module-notes.md` documenting client transfer orchestration boundaries
- [X] T005 Add planned source/header placeholders to `CMakeLists.txt` for `modules/cluster`, `modules/view`, `modules/store/transfer`, and new app targets without changing existing target names
- [X] T006 Add planned CTest labels and guarded test target entries for 008 tests in `tests/CMakeLists.txt`
- [X] T007 [P] Create test helper ownership notes for integrated cluster helpers in `tests/support/integrated_cluster_test_utils.h`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: 所有 user story 都依赖的配置、身份、ViewNode 契约和 payload 边界基础。

**CRITICAL**: No user story work can begin until this phase is complete.

- [X] T008 [P] Add additive ViewNode protobuf contract in `proto/view.proto` with RegisterNode, HeartbeatNode, DiscoverMetadata, DiscoverStorage, and GetClusterView messages
- [X] T009 Update root `CMakeLists.txt` to generate and link `view_proto` while preserving existing `raft_proto`, `metadata_proto`, and `storage_node_proto` semantics
- [X] T010 [P] Define cluster config data structures and validation interfaces in `modules/cluster/cluster_config.h`
- [X] T011 Implement cluster config validation and deterministic generation in `modules/cluster/cluster_config.cpp`
- [X] T012 [P] Define durable node identity types and load/store interfaces in `modules/cluster/node_identity.h`
- [X] T013 Implement durable `node.identity` load/store with explicit Linux/Windows durability contracts in `modules/cluster/node_identity.cpp`
- [X] T014 [P] Add node identity unit tests in `tests/node_identity_test.cpp`
- [X] T015 [P] Define ViewNode registry and role/status types in `modules/view/view_registry.h`
- [X] T016 Implement ViewNode registry registration, heartbeat sequencing, liveness transitions, and discovery snapshots in `modules/view/view_registry.cpp`
- [X] T017 [P] Add ViewNode registry unit tests in `tests/view_node_discovery_test.cpp`
- [X] T018 Add ViewNode gRPC service adapter declarations in `modules/view/view_service_impl.h`
- [X] T019 Implement ViewNode gRPC service adapter in `modules/view/view_service_impl.cpp`
- [X] T020 [P] Define ViewNode client adapter in `modules/view/view_client.h`
- [X] T021 Implement ViewNode client adapter in `modules/view/view_client.cpp`
- [X] T022 [P] Add payload boundary audit test in `tests/integrated_object_storage_e2e_test.cpp` ensuring metadata commands/manifests never include raw file payload
- [X] T023 Replace full-object buffering checksum path with streaming/bounded checksum interfaces in `modules/store/upload/upload_coordinator.h`
- [X] T024 Implement streaming/bounded checksum behavior in `modules/store/upload/upload_coordinator.cpp`
- [X] T025 Update upload coordinator tests for bounded checksum behavior in `tests/storage_upload_coordinator_test.cpp`

**Checkpoint**: Foundation ready - config, identity, ViewNode contract, discovery registry, and payload boundary tests are in place.

---

## Phase 3: User Story 1 - 上传并下载真实对象 (Priority: P1) MVP

**Goal**: 用户能上传真实文件、通过 Raft commit metadata、从 StorageNode 下载并验证 SHA-256 完全一致。

**Independent Test**: 启动最小可运行集群，上传 64 MiB 文件，下载到新路径，比对 SHA-256，确认对象只有 COMMITTED 后可见。

### Tests for User Story 1

- [X] T026 [P] [US1] Add E2E upload/download happy-path test scaffold in `tests/integrated_object_storage_e2e_test.cpp`
- [X] T027 [P] [US1] Add manifest visibility test for PENDING hidden and COMMITTED visible states in `tests/integrated_object_storage_e2e_test.cpp`
- [X] T028 [P] [US1] Add checksum mismatch download failure test in `tests/integrated_object_storage_e2e_test.cpp`

### Implementation for User Story 1

- [X] T029 [P] [US1] Define transfer session, chunk reader, and checksum state interfaces in `modules/store/transfer/object_transfer.h`
- [X] T030 [US1] Implement bounded file chunking and upload transfer session in `modules/store/transfer/object_transfer.cpp`
- [X] T031 [P] [US1] Define metadata upload/download adapter interface in `modules/store/transfer/metadata_transfer_client.h`
- [X] T032 [US1] Implement metadata transfer adapter over MetadataService in `modules/store/transfer/metadata_transfer_client.cpp`
- [X] T033 [P] [US1] Define StorageNode chunk read/write adapter interface in `modules/store/transfer/storage_transfer_client.h`
- [X] T034 [US1] Implement StorageNode chunk read/write adapter in `modules/store/transfer/storage_transfer_client.cpp`
- [X] T035 [US1] Integrate ViewNode discovery into transfer orchestration in `modules/store/transfer/object_transfer.cpp`
- [X] T036 [US1] Add manifest-driven download reconstruction and final SHA-256 verification in `modules/store/transfer/object_transfer.cpp`
- [X] T037 [US1] Implement `storage_client upload` and `storage_client download` commands in `apps/storage_client.cpp`
- [X] T038 [US1] Wire `storage_client` target and dependencies in `CMakeLists.txt`
- [X] T039 [US1] Update `tests/CMakeLists.txt` to build and label `integrated_object_storage_e2e`

**Checkpoint**: User Story 1 is independently demonstrable with real upload/download and checksum verification.

---

## Phase 4: User Story 2 - 配置驱动启动完整集群 (Priority: P1)

**Goal**: 用户通过统一配置启动可变数量 ViewNode、MetadataNode、StorageNode，不依赖硬编码端口、路径或节点数量。

**Independent Test**: 生成 1/3/5 MetadataNode 和多个 StorageNode 的配置，启动 app 时读取配置，检查 quorum 和注册状态。

### Tests for User Story 2

- [ ] T040 [P] [US2] Add cluster config generation tests for 1/3/5/7 Raft voters in `tests/cluster_config_test.cpp`
- [ ] T041 [P] [US2] Add app config parsing smoke tests in `tests/integrated_object_storage_e2e_test.cpp`

### Implementation for User Story 2

- [ ] T042 [US2] Implement per-node config resolution and endpoint allocation in `modules/cluster/cluster_config.cpp`
- [ ] T043 [US2] Add quorum calculation helpers based on initial Raft voters in `modules/cluster/cluster_config.cpp`
- [ ] T044 [P] [US2] Implement config generator command in `apps/storage_client.cpp`
- [ ] T045 [P] [US2] Implement thin `view_node_app` startup in `apps/view_node_app.cpp`
- [ ] T046 [P] [US2] Implement thin `metadata_node_app` startup in `apps/metadata_node_app.cpp`
- [ ] T047 [P] [US2] Implement thin `storage_node_app` startup in `apps/storage_node_app.cpp`
- [ ] T048 [US2] Wire `view_node_app`, `metadata_node_app`, and `storage_node_app` targets in `CMakeLists.txt`
- [ ] T049 [US2] Update `quickstart.md` command examples after final app argument names are implemented in `specs/008-integrated-object-storage-system/quickstart.md`

**Checkpoint**: User Story 2 can start a configurable local cluster without code changes.

---

## Phase 5: User Story 5 - 遵守 Raft quorum 安全边界 (Priority: P1)

**Goal**: Raft MetadataNode commit 和 election 始终使用已提交 voter membership 的 majority quorum，不能按当前 live 节点降低。

**Independent Test**: 3 voter 集群停 2 个节点后无法 commit 新对象；5 voter 集群 quorum 为 3；ViewNode 注册新 Raft 节点不改变 voter quorum。

### Tests for User Story 5

- [X] T050 [P] [US5] Add 3-voter quorum insufficiency object commit test in `tests/integrated_object_storage_quorum_test.cpp`
- [X] T051 [P] [US5] Add 5-voter quorum calculation and commit availability test in `tests/integrated_object_storage_quorum_test.cpp`
- [X] T052 [P] [US5] Add ViewNode-registered Raft node not counted as voter test in `tests/integrated_object_storage_quorum_test.cpp`

### Implementation for User Story 5

- [X] T053 [US5] Expose read-only committed membership/quorum summary for diagnostics in `modules/raft/node/raft_node.h`
- [X] T054 [US5] Implement quorum summary without changing election or commit behavior in `modules/raft/node/raft_node.cpp`
- [ ] T055 [US5] Map quorum and leader diagnostics into MetadataService responses where needed in `modules/raft/service/metadata_service_impl.cpp`
- [X] T056 [US5] Add ViewNode Raft observation status mapping without membership authority in `modules/view/view_registry.cpp`
- [ ] T057 [US5] Wire `integrated_object_storage_quorum` test target in `tests/CMakeLists.txt`

**Checkpoint**: User Story 5 proves no availability shortcut can shrink Raft quorum.

---

## Phase 6: User Story 3 - 服务发现与节点状态观测 (Priority: P2)

**Goal**: ViewNode 支持节点注册、发现、健康状态、容量、心跳和 leader hint 观测，Client 不硬编码 MetadataNode 地址。

**Independent Test**: MetadataNode/StorageNode 注册后可查询；StorageNode 停止心跳后变为 stale/dead；新的 placement 不选择 dead 节点。

### Tests for User Story 3

- [ ] T058 [P] [US3] Add ViewNode discovery integration tests for metadata and storage endpoints in `tests/view_node_discovery_test.cpp`
- [ ] T059 [P] [US3] Add heartbeat timeout and liveness transition tests in `tests/view_node_discovery_test.cpp`
- [ ] T060 [P] [US3] Add placement excludes dead ViewNode-observed StorageNode test in `tests/store_placement_manager_test.cpp`

### Implementation for User Story 3

- [ ] T061 [US3] Add node registration client loop for MetadataNode startup in `apps/metadata_node_app.cpp`
- [ ] T062 [US3] Add node registration and heartbeat loop for StorageNode startup in `apps/storage_node_app.cpp`
- [ ] T063 [US3] Add ViewNode-backed StorageNode snapshot adapter for placement in `modules/store/placement/placement_manager.h`
- [ ] T064 [US3] Implement ViewNode-backed StorageNode snapshot adapter in `modules/store/placement/placement_manager.cpp`
- [ ] T065 [US3] Add `storage_client status` command using ViewNode cluster view in `apps/storage_client.cpp`
- [ ] T066 [US3] Add leader hint refresh and NOT_LEADER retry boundary in `modules/store/transfer/metadata_transfer_client.cpp`

**Checkpoint**: User Story 3 provides discovery and observation without turning ViewNode into a consistency authority.

---

## Phase 7: User Story 4 - 节点身份自动分配并持久化 (Priority: P2)

**Goal**: StorageNode/ViewNode/MetadataNode 使用稳定 node_id，重启后身份不变，identity 冲突可诊断。

**Independent Test**: 首次启动写 identity；同 data_dir 重启复用；损坏或冲突 identity 失败； MetadataNode raft_id 来自配置生成。

### Tests for User Story 4

- [ ] T067 [P] [US4] Add StorageNode first-start identity allocation test in `tests/node_identity_test.cpp`
- [ ] T068 [P] [US4] Add restart reuses node_id test in `tests/node_identity_test.cpp`
- [ ] T069 [P] [US4] Add identity/config mismatch failure test in `tests/node_identity_test.cpp`
- [ ] T070 [P] [US4] Add MetadataNode raft_id generated by config test in `tests/cluster_config_test.cpp`

### Implementation for User Story 4

- [ ] T071 [US4] Integrate identity load/create into `apps/storage_node_app.cpp`
- [ ] T072 [US4] Integrate identity load/create into `apps/view_node_app.cpp`
- [ ] T073 [US4] Integrate config-generated node_id and raft_id validation into `apps/metadata_node_app.cpp`
- [ ] T074 [US4] Add ViewNode node_id allocation path for StorageNode first registration in `modules/view/view_service_impl.cpp`
- [ ] T075 [US4] Add durable identity conflict diagnostics in `modules/cluster/node_identity.cpp`

**Checkpoint**: User Story 4 proves node identity stability across restarts.

---

## Phase 8: User Story 6 - 故障、恢复与并发读写 (Priority: P3)

**Goal**: 系统在 StorageNode 故障、重启、未 commit 清理、并发上传下载和大文件传输下仍保持可见性和 checksum 正确。

**Independent Test**: 运行 recovery/concurrency 测试矩阵，并检查每个成功 commit 对象都能通过最终 SHA-256。

### Tests for User Story 6

- [ ] T076 [P] [US6] Add StorageNode restart after committed upload test in `tests/integrated_object_storage_recovery_test.cpp`
- [ ] T077 [P] [US6] Add uncommitted chunk cleanup test in `tests/integrated_object_storage_recovery_test.cpp`
- [ ] T078 [P] [US6] Add concurrent upload/download stress test with 100 operations in `tests/integrated_object_storage_concurrency_test.cpp`
- [ ] T079 [P] [US6] Add no healthy StorageNode capacity failure test in `tests/integrated_object_storage_recovery_test.cpp`

### Implementation for User Story 6

- [ ] T080 [US6] Add orphan/staging cleanup integration hook in `modules/store/maintenance/garbage_collector.cpp`
- [ ] T081 [US6] Add cleanup candidate emission from failed upload sessions in `modules/store/transfer/object_transfer.cpp`
- [ ] T082 [US6] Add retry/backoff policy for transient StorageNode failures in `modules/store/transfer/storage_transfer_client.cpp`
- [ ] T083 [US6] Add bounded concurrency controls for upload/download sessions in `modules/store/transfer/object_transfer.cpp`
- [ ] T084 [US6] Wire recovery and concurrency test targets in `tests/CMakeLists.txt`
- [ ] T085 [US6] Record Linux-specific failure validation and Windows fallback notes in `specs/008-integrated-object-storage-system/risk-register.md`

**Checkpoint**: User Story 6 validates failure and concurrency behavior beyond happy path.

---

## Final Phase: Polish & Cross-Cutting Concerns

**Purpose**: 收口文档、诊断、跨平台和验收矩阵。

- [ ] T086 [P] Add or update module notes for `modules/cluster/module-notes.md`, `modules/view/module-notes.md`, and `modules/store/transfer/module-notes.md`
- [ ] T087 [P] Add validation matrix for acceptance scenarios in `specs/008-integrated-object-storage-system/validation-matrix.md`
- [ ] T088 [P] Add Windows startup and path smoke notes to `specs/008-integrated-object-storage-system/quickstart.md`
- [ ] T089 Add request_id/node_id/leader-hint diagnostic consistency checks across `apps/storage_client.cpp`, `modules/view/view_service_impl.cpp`, and `modules/raft/service/metadata_service_impl.cpp`
- [ ] T090 Run `cmake --preset debug-ninja-low-parallel` and save only failure summaries to `specs/008-integrated-object-storage-system/task-reports/` if needed
- [ ] T091 Run `cmake --build --preset debug-ninja-low-parallel` and save only failure summaries to `specs/008-integrated-object-storage-system/task-reports/` if needed
- [ ] T092 Run `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all` and report according to Test Log Output Rules

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup completion and blocks all user stories.
- **US1 Upload/Download (Phase 3)**: Depends on Foundational.
- **US2 Config Startup (Phase 4)**: Depends on Foundational; can proceed in parallel with US1 after shared app target decisions.
- **US5 Quorum Safety (Phase 5)**: Depends on Foundational and can proceed in parallel with US1/US2, but must complete before broad E2E claims.
- **US3 Discovery (Phase 6)**: Depends on Foundational and benefits from US2 app startup wiring.
- **US4 Identity (Phase 7)**: Depends on Foundational and should complete before StorageNode restart acceptance.
- **US6 Recovery/Concurrency (Phase 8)**: Depends on US1, US2, US3, US4, and US5 MVP behavior.
- **Polish**: Depends on selected stories being complete.

### User Story Dependencies

- **US1 (P1)**: MVP upload/download; no dependency on US3 beyond foundational discovery client.
- **US2 (P1)**: Config-driven startup; independent from object semantics once foundational config exists.
- **US5 (P1)**: Quorum safety; independent test slice but required before any production-like claim.
- **US3 (P2)**: Full discovery/observability; extends foundational ViewNode.
- **US4 (P2)**: Stable identity; extends foundational identity and app startup.
- **US6 (P3)**: Failure/concurrency; depends on the integrated system slices.

### Within Each User Story

- Tests for touched high-risk paths should be written or updated before finalizing implementation.
- Proto contract updates must precede generated target and service/client adapters.
- Config/identity validation must precede app startup integration.
- StorageNode durability and cleanup hooks must precede recovery acceptance.
- Diagnostics and module notes must be updated before closing the story.

### Parallel Opportunities

- T002, T003, T004, and T007 can run in parallel.
- T010/T012/T015/T020/T022 can run in parallel after proto shape is agreed.
- US1 tests T026-T028 can run in parallel.
- US2 app entry tasks T045-T047 can run in parallel after config loader is available.
- US5 tests T050-T052 can run in parallel.
- US3 tests T058-T060 can run in parallel.
- US4 tests T067-T070 can run in parallel.
- US6 tests T076-T079 can run in parallel after E2E helper exists.

---

## Parallel Example: User Story 1

```bash
# Tests can be authored together:
Task: "T026 [US1] Add E2E upload/download happy-path test scaffold in tests/integrated_object_storage_e2e_test.cpp"
Task: "T027 [US1] Add manifest visibility test for PENDING hidden and COMMITTED visible states in tests/integrated_object_storage_e2e_test.cpp"
Task: "T028 [US1] Add checksum mismatch download failure test in tests/integrated_object_storage_e2e_test.cpp"

# Independent adapters can be implemented together:
Task: "T031 [US1] Define metadata upload/download adapter interface in modules/store/transfer/metadata_transfer_client.h"
Task: "T033 [US1] Define StorageNode chunk read/write adapter interface in modules/store/transfer/storage_transfer_client.h"
```

## Parallel Example: User Story 2

```bash
Task: "T045 [US2] Implement thin view_node_app startup in apps/view_node_app.cpp"
Task: "T046 [US2] Implement thin metadata_node_app startup in apps/metadata_node_app.cpp"
Task: "T047 [US2] Implement thin storage_node_app startup in apps/storage_node_app.cpp"
```

## Implementation Strategy

### MVP First

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational config/identity/ViewNode/payload boundary work.
3. Complete US1 upload/download.
4. Complete US5 quorum safety before presenting any strong-consistency claim.
5. Validate with `integrated_object_storage_e2e` and `integrated_object_storage_quorum`.

### Incremental Delivery

1. Foundation: config, identity, ViewNode registry, contracts.
2. MVP: real upload/download and checksum verification.
3. Configurable apps: independent ViewNode/MetadataNode/StorageNode/client startup.
4. Discovery and identity hardening.
5. Recovery and concurrency validation.

### Risk Controls

- Keep app files thin and move orchestration into modules.
- Treat any change to `proto/`, `modules/raft/node`, `modules/raft/storage`, and `modules/raft/service` as high risk.
- Never reduce quorum or infer membership from ViewNode registration.
- Never place real payload into metadata or Raft persistence.
- Record cross-task design risks in `specs/008-integrated-object-storage-system/risk-register.md` or `task-reports/`, not in high-frequency docs.
