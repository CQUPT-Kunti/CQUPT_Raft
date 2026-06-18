# Implementation Plan: Local RPC Object Storage Stabilization

**Branch**: `009-local-rpc-object-storage-stabilization` | **Date**: 2026-06-11 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/009-local-rpc-object-storage-stabilization/spec.md`

## Summary

009 阶段把 008 已经跑通的“配置内静态启动 local RPC 对象存储原型”推进到“动态节点加入 + 多 ViewNode 高可用发现”的稳定化阶段。核心技术方向是：修正 ViewNode self refresh；引入 ViewNode active-active observed registry 同步；让 StorageNode 运行中注册并参与后续 placement；让 MetadataNode 运行中只能先通过 Raft committed membership 加入为 learner，再按奇数 voter 约束安全批量 promote；持续保持 ViewNode 只负责 discovery / observation，Raft committed configuration log 才是 membership authority。

本阶段不直接把“注册到 ViewNode”简化为“加入集群”。StorageNode 注册是服务发现问题，ViewNode 注册是观测同步问题，Metadata/RaftNode 加入是共识成员变更问题。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: gRPC, Protobuf, GoogleTest, CMake, standard library  
**Storage**: Raft metadata under `NodeConfig::data_dir` and `snapshotConfig::snapshot_dir`; StorageNode chunks under StorageNode data dir; local persistent node identity in `node.identity`; 009 must define ViewNode registry persistence or restart recovery boundary before peer sync is considered complete  
**Testing**: GoogleTest + CTest labels + targeted local RPC example scripts; no standalone compiler invocation for normal validation  
**Target Platform**: Linux primary validation; Windows/macOS design-compatible with explicit pending smoke/fallback where no real machine is available  
**Project Type**: Local RPC distributed object storage prototype with Raft metadata control-plane, StorageNode data-plane, ViewNode discovery/observation plane  
**Performance Goals**: Dynamic heartbeat / peer sync must not block normal discovery or Raft metadata commit paths; learner catch-up must continue replicating log/snapshot while waiting for odd-voter-safe promote; object payload remains bounded by chunk size and concurrency  
**Constraints**: Preserve existing protocol semantics, persisted formats, public API behavior, class/function names, and verified 008 static local RPC roundtrip unless explicitly scoped; required durability operations cannot silently degrade  
**Scale/Scope**: 2 ViewNodes, 3 initial Metadata voters, dynamic 1/2 Metadata learners, multiple StorageNodes, running-time StorageNode join, local RPC failover and restart matrix  

## Current Baseline From Report And Inspection

### Report-Confirmed Local RPC Baseline

- Report: `specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`
- Current example: `examples/object-storage-local-3meta-6store`
- Current real RPC topology: 1 ViewNode, 3 MetadataNode, 6 StorageNode
- Current client: `storage_client`
- Current test file source directory: `tests/test_file`
- Current app targets used by report validation:
  - `view_node_app`
  - `metadata_node_app`
  - `storage_node_app`
  - `storage_client`
  - `raft_metadata_client`
- Report-confirmed scripts:
  - `examples/object-storage-local-3meta-6store/qidong.sh`
  - `examples/object-storage-local-3meta-6store/tingzhi.sh`
  - `examples/object-storage-local-3meta-6store/rpc_demo.sh`
- Report-confirmed validated path:
  - `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp`
- Report-confirmed files changed by the 008 stabilization pass:
  - `apps/metadata_node_app.cpp`
  - `modules/raft/service/metadata_service_impl.cpp`
  - `modules/store/transfer/object_transfer.cpp`
- Report-confirmed remaining issue:
  - ViewNode self-liveness may show `stale/dead` because ViewNode self registry record is not continuously refreshed.

### Additional Inspection-Confirmed Entry Points

- Example startup:
  - `examples/object-storage-local-3meta-6store/qidong.sh` starts `view-1`, `meta-1..meta-3`, and `store-1..store-6`.
  - `examples/object-storage-local-3meta-6store/tingzhi.sh` stops those nodes in reverse order.
  - `examples/object-storage-local-3meta-6store/rpc_demo.sh status|upload|download|roundtrip` uses `storage_client` and `tests/test_file`.
  - `examples/object-storage-local-3meta-6store/cluster.json` fixes 1 ViewNode at `127.0.0.1:7301`, 3 metadata voters at `127.0.0.1:7401..7403`, 6 StorageNodes at `127.0.0.1:7501..7506`, initial voters `[1,2,3]`, no learners.
- Current app startup paths:
  - `apps/view_node_app.cpp`
  - `apps/metadata_node_app.cpp`
  - `apps/storage_node_app.cpp`
  - `apps/storage_client.cpp`
- Current identity paths:
  - `modules/cluster/node_identity.h`
  - `modules/cluster/node_identity.cpp`
  - `tests/node_identity_test.cpp`
- Current cluster config paths:
  - `modules/cluster/cluster_config.h`
  - `modules/cluster/cluster_config.cpp`
  - `tests/cluster_config_test.cpp`
- Current ViewNode paths:
  - `modules/view/view_registry.h`
  - `modules/view/view_registry.cpp`
  - `modules/view/view_service_impl.h`
  - `modules/view/view_service_impl.cpp`
  - `modules/view/view_client.h`
  - `modules/view/view_client.cpp`
  - `tests/view_node_discovery_test.cpp`
- Current StorageNode registry / registration / heartbeat paths:
  - `modules/store/node/storage_node_registry.h`
  - `modules/store/node/storage_node_registry.cpp`
  - `modules/store/node/storage_node_service.h`
  - `modules/store/node/storage_node_service.cpp`
  - `modules/store/node/storage_node_client.h`
  - `modules/store/node/storage_node_client.cpp`
  - `tests/storage_heartbeat_registry_test.cpp`
  - `tests/storage_node_service_test.cpp`
  - `tests/storage_node_client_test.cpp`
- Current placement and transfer paths:
  - `modules/store/placement/placement_manager.h`
  - `modules/store/placement/placement_manager.cpp`
  - `modules/store/transfer/object_transfer.cpp`
  - `modules/store/transfer/metadata_transfer_client.cpp`
  - `modules/store/transfer/storage_transfer_client.cpp`
  - `tests/integrated_object_storage_e2e_test.cpp`
- Current Metadata authority / leader / client tests:
  - `modules/raft/service/metadata_service_impl.cpp`
  - `tests/metadata_failover_test.cpp`
  - `tests/metadata_client_scenario_test.cpp`
  - `tests/integrated_object_storage_e2e_test.cpp`
- Current Raft membership / quorum / replication / snapshot tests:
  - `modules/raft/node/raft_node.h`
  - `modules/raft/node/raft_node.cpp`
  - `modules/raft/replication/replicator.h`
  - `modules/raft/replication/replicator.cpp`
  - `tests/test_raft_election.cpp`
  - `tests/test_raft_log_replication.cpp`
  - `tests/test_raft_commit_apply.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/integrated_object_storage_quorum_test.cpp`
- Current CMake target / label entry:
  - Root app target wiring in `CMakeLists.txt`.
  - CTest labels and custom test targets in `tests/CMakeLists.txt`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Verified existing capabilities affected by this feature are identified and excluded from unnecessary replanning.
  - PASS: 008 static local RPC roundtrip, Metadata authority fixes, StorageNode chunk writes, and committed-membership quorum diagnostics are protected baselines.
- Any protocol, public API, or persisted format change is either absent or explicitly justified with regression coverage and explicit schema rules.
  - PASS with scoped risk: 009 likely needs scoped discovery / membership contracts such as ViewNode peer sync and Metadata join; existing semantics must remain stable. For `node.identity`, 009 uses a new-only schema: old-format or missing-required-field files must fail fast, and future migration must be a separate task.
- Durability, crash-recovery, and restart-recovery implications are stated for every affected path in `node`, `replication`, `storage`, or `state_machine`.
  - PASS with required tasks: identity persistence already exists; ViewNode registry persistence/recovery boundary and learner membership commit recovery must be specified before implementation.
- Linux-specific validation is explicitly labeled, and Windows/macOS fallback, adaptation, or deferred follow-up is recorded.
  - PASS: Linux local RPC and CTest validation are primary; Windows/macOS identity/durability/startup are marked fallback or pending when not executable locally.
- Test entry points are defined through CTest plus any justified platform-specific script or preset additions.
  - PASS: target tests and example scripts are enumerated from existing files.
- Observability and diagnostics impact is captured for high-risk work.
  - PASS: 009 requires request_id/node_id/incarnation/sequence/membership-state diagnostics and task reports under feature task-reports.

## Project Structure

### Documentation (this feature)

```text
specs/009-local-rpc-object-storage-stabilization/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── validation-matrix.md
├── cross-task-risk-notes.md
├── module-notes.md
├── checklists/
│   └── requirements.md
├── contracts/
│   ├── identity-lifecycle.md
│   ├── view-node-self-refresh-and-peer-sync.md
│   ├── storage-dynamic-join.md
│   ├── metadata-learner-join.md
│   └── local-rpc-validation.md
├── task-reports/
│   └── local-rpc-object-storage-stabilization-report.md
└── tasks.md
```

### Source Code (repository root)

```text
apps/
├── view_node_app.cpp
├── metadata_node_app.cpp
├── storage_node_app.cpp
├── storage_client.cpp
└── raft_metadata_client.cpp

proto/
├── view.proto
├── raft.proto
├── metadata.proto
└── storage_node.proto

modules/
├── cluster/
│   ├── cluster_config.h
│   ├── cluster_config.cpp
│   ├── node_identity.h
│   └── node_identity.cpp
├── view/
│   ├── view_registry.h
│   ├── view_registry.cpp
│   ├── view_service_impl.h
│   ├── view_service_impl.cpp
│   ├── view_client.h
│   └── view_client.cpp
├── store/
│   ├── node/
│   ├── placement/
│   └── transfer/
└── raft/
    ├── node/
    ├── replication/
    ├── service/
    ├── storage/
    └── state_machine/

tests/
├── view_node_discovery_test.cpp
├── storage_heartbeat_registry_test.cpp
├── node_identity_test.cpp
├── cluster_config_test.cpp
├── integrated_object_storage_e2e_test.cpp
├── integrated_object_storage_quorum_test.cpp
├── integrated_object_storage_recovery_test.cpp
├── integrated_object_storage_concurrency_test.cpp
├── metadata_failover_test.cpp
├── metadata_client_scenario_test.cpp
├── test_raft_election.cpp
├── test_raft_log_replication.cpp
├── test_raft_commit_apply.cpp
├── test_raft_snapshot_catchup.cpp
└── test_raft_snapshot_restart.cpp

examples/
└── object-storage-local-3meta-6store/
    ├── cluster.json
    ├── qidong.sh
    ├── tingzhi.sh
    └── rpc_demo.sh
```

**Structure Decision**: Reuse existing module boundaries. ViewNode self refresh / peer sync belongs in `modules/view` and `apps/view_node_app.cpp`. Identity lifecycle additions belong in `modules/cluster`. StorageNode dynamic join belongs in `apps/storage_node_app.cpp`, `modules/view`, `modules/store/node`, and `modules/store/placement`. Metadata learner join and odd-voter-safe promote belong in `proto/`, `modules/raft/node`, `modules/raft/replication`, `modules/raft/service`, and related tests. App files must stay thin and delegate business logic to modules.

## Phase 0: Research

Research output is captured in [research.md](research.md). Decisions:

- `identity_file` is local persistent identity, not a pre-created requirement and not ViewNode-assigned identity.
- ViewNode registry is currently in-memory; 009 must define persistence/restart boundary for peer sync.
- ViewNode currently self-registers once and lacks self refresh loop.
- StorageNode currently heartbeats to one active ViewNode, while MetadataNode heartbeats to all configured ViewNodes.
- Current Raft runtime membership is config-derived voters in `NodeConfig::peers`; `CommittedMembershipQuorumSummary` is diagnostic only and learners are not runtime members.
- Dynamic MetadataNode join requires new committed membership machinery; ViewNode observed membership cannot be authority.
- Odd committed voter count is a hard invariant; single ready learner remains pending when promote would create even voters.

## Phase 1: Design & Contracts

Design artifacts:

- [data-model.md](data-model.md): identity, process incarnation, observed state, ViewNode peer snapshot, StorageNode dynamic registration, Metadata join, learner catch-up, batch membership change.
- [contracts/identity-lifecycle.md](contracts/identity-lifecycle.md): first start, restart reuse, mismatch, corruption, bootstrap voter, dynamic join candidate.
- [contracts/view-node-self-refresh-and-peer-sync.md](contracts/view-node-self-refresh-and-peer-sync.md): self refresh, peer registry sync, merge order, old incarnation protection.
- [contracts/storage-dynamic-join.md](contracts/storage-dynamic-join.md): discovery-only StorageNode join and placement visibility.
- [contracts/metadata-learner-join.md](contracts/metadata-learner-join.md): learner join, catch-up, odd voter block, batch promote.
- [contracts/local-rpc-validation.md](contracts/local-rpc-validation.md): 009 local RPC example and validation matrix entry points.
- [quickstart.md](quickstart.md): targeted build and manual validation guidance.

## Post-Design Constitution Check

- Preserve verified core: PASS. 008 local RPC roundtrip, Metadata authority behavior, committed quorum diagnostics, and StorageNode data-plane semantics are protected.
- Protocol/public API/persisted format: PASS with scoped changes. New join / peer sync contracts must preserve existing semantics. For `node.identity`, 009 does not keep legacy compatibility or automatic upgrade; if a future deployed migration is needed, it must be specified in a separate task.
- Durability/recovery: PASS with required coverage. Identity, registry snapshot/restart, learner catch-up, and membership log commit recovery are explicit tasks.
- Cross-platform: PASS. Linux validation is primary; Windows/macOS behavior is recorded as smoke/pending where not executable.
- Observability/minimal surface: PASS. Tasks emphasize request_id, node_id, incarnation, sequence, membership state, quorum summary, and task reports.

## Implementation Phases

### Phase 1: Existing Local RPC / Test / Report Survey And Spec Landing

- Confirm report facts, example scripts, app targets, CTest target/label, identity and membership state.
- Produce 009 spec, plan, research, data model, contracts, quickstart, tasks, and validation matrix.

### Phase 2: identity_file / Node Identity Lifecycle Closure

- Define identity_file semantics for first start and restart.
- Add process incarnation / boot epoch and heartbeat sequence responsibilities.
- Separate StorageNode, ViewNode, Metadata bootstrap voter, and Metadata dynamic join identity flows.

### Phase 3: ViewNode Self State Refresh And Liveness Fix

- Add ViewNode self refresh independent of external heartbeats.
- Keep healthy ViewNode `LIVE` beyond TTL.
- Add deterministic tests with controllable time source.

### Phase 4: ViewNode Incarnation / Sequence / Registry Merge Protection

- Add incarnation-aware observed state and merge ordering.
- Prevent old heartbeat or old registry snapshot from overwriting new process state.

### Phase 5: Multi-ViewNode Peer Registry Sync

- Support at least two ViewNodes.
- Add peer seed configuration and Push/Pull/Merge observed registry sync.
- Keep active-active discovery eventually consistent and non-authoritative.

### Phase 6: StorageNode Dynamic Registration And Placement Visibility

- Allow running-time StorageNode startup with local identity creation.
- Register/heartbeat to ViewNode seed(s).
- Make new StorageNode visible to subsequent placement without rebalance.

### Phase 7: MetadataNode Dynamic Learner Join Interface And Safety Constraints

- Add dynamic join mode.
- Register observed metadata facts with ViewNode.
- Discover leader via ViewNode and send join request to Metadata leader.
- Commit AddLearner membership through Raft, not ViewNode.

### Phase 8: Learner Catch-Up / Pending Learner / Odd Voter Constraint

- Ensure learner receives AppendEntries / InstallSnapshot and advances catch-up progress.
- Block single learner promote when voter count would become even.
- Keep quorum/election based on committed voters only.

### Phase 9: Batch Promote / Joint Consensus / Batched Membership Change

- Implement safe promotion from 3 voters + 2 ready learners to 5 voters without committed 4 voters.
- Handle leader failure during batch promote without inconsistent membership.

### Phase 10: Local RPC Example Extension And End-To-End Validation

- Extend or add example for 2 ViewNodes + 3 initial Metadata voters + multiple StorageNodes.
- Validate dynamic StorageNode join, 1 learner pending, 2 learners batch promote, ViewNode failover, identity restart.

### Phase 11: Failure / Restart / Duplicate / TTL / Old Incarnation / Odd Voter Tests

- Complete stability matrix across ViewNode, StorageNode, Metadata/Raft, identity, TTL, stale heartbeat, stale snapshot, duplicate join, leader change.

### Phase 12: Documentation, Reports, Acceptance Matrix Closure

- Update feature docs, task reports, validation matrix, cross-task risks, and module notes.
- Summarize Linux validation and Windows/macOS pending items.

## Required Validation Strategy

- Do not default to full build.
- Prefer target builds such as:
  - `cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client`
  - `cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e integrated_object_storage_quorum test_view_node_discovery test_node_identity`
- Use CTest labels and target tests from `tests/CMakeLists.txt`:
  - `integrated-object-storage-e2e`
  - `integrated-object-storage-quorum`
  - `integrated-object-storage-recovery`
  - `integrated-object-storage-concurrency`
  - `view-node`
  - `node-identity`
  - `storage-node`
- For local RPC validation, keep logs local and summarize only:
  - `examples/object-storage-local-3meta-6store/qidong.sh`
  - `examples/object-storage-local-3meta-6store/rpc_demo.sh status`
  - `examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`
  - `examples/object-storage-local-3meta-6store/tingzhi.sh`
- For dynamic join validation, tests must prove nodes are added during runtime, not statically configured before one-shot startup.
- In concurrent build windows, use a local build lock. If the lock cannot be acquired, skip build/test and record the skip in `specs/009-local-rpc-object-storage-stabilization/task-reports/`.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Add ViewNode peer sync contract | 009 requires at least two ViewNodes to avoid discovery single point | Single ViewNode heartbeat fix would not satisfy active-active discovery |
| Add process incarnation / boot epoch to observed state | Sequence alone cannot distinguish restarted process instances | observed_time-only merge allows old process state to overwrite new state |
| Add Raft dynamic learner membership | MetadataNode dynamic join is consensus membership, not discovery registration | Registering MetadataNode to ViewNode would bypass Raft safety |
| Add batch promote / joint consensus or equivalent | Odd committed voter count forbids 3 -> 4 -> 5 transition | Single learner promote would commit an even voter count |
| Add registry persistence/restart boundary | Multi ViewNode sync needs restart semantics and stale snapshot protection | Pure in-memory sync loses state after restart and cannot define old snapshot merge safety |
