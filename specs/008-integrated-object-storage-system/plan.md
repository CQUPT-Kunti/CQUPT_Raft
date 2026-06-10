# Implementation Plan: Integrated Object Storage System

**Branch**: `008-integrated-object-storage-system` | **Date**: 2026-06-05 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/008-integrated-object-storage-system/spec.md`

## Summary

本阶段把已经具备强一致对象元数据能力的 Raft metadata control-plane 与已经具备真实 chunk 能力的 StorageNode data-plane 连接为可端到端运行的对象存储雏形。核心技术方向是：新增 ViewNode 服务发现与观测边界、统一配置与节点身份持久化、用 MetadataNode 生成 WritePlan/manifest 并通过 Raft quorum commit、用 StorageNode 保存真实 payload、用 storage_client 执行真实文件上传下载和 checksum 验收。第一阶段明确不实现运行时 Raft voter 动态变更；Raft 新节点注册到 ViewNode 只代表被发现和观测，membership 权威仍然只能是 Raft 已提交配置。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: gRPC, Protobuf, GoogleTest, CMake, standard library  
**Storage**: Raft metadata persists under `NodeConfig::data_dir` / `snapshotConfig::snapshot_dir`; StorageNode chunks persist under StorageNode data_dir with staging -> durable flush -> publish semantics; node identity persists as `node.identity` under each node data_dir  
**Testing**: GoogleTest + CTest labels, `./test.sh --group unit|persistence|all`, feature-specific end-to-end executable tests, Linux-primary failure/restart validation, Windows smoke/adaptation tasks  
**Target Platform**: Linux primary validation; Windows supported startup/config/path/durability design target; macOS design-compatible but not first-stage acceptance gate  
**Project Type**: Cross-platform Raft metadata control-plane plus StorageNode data-plane distributed object storage prototype  
**Performance Goals**: Real file upload/download must be chunked; memory use bounded by chunk size and concurrency; at least 100 concurrent client operations in acceptance stress; no full payload stored in Raft log/snapshot  
**Constraints**: Preserve existing Raft safety, majority commit, leader election, persisted formats, StorageNode durability semantics, proto field stability, module boundaries, and no-KV main path. New service contracts are allowed only when explicitly scoped for ViewNode/config/client integration and must not alter existing RPC semantics.  
**Scale/Scope**: Initial Raft voter membership is config-generated for 1/3/5/7 nodes; ViewNode count is configurable but first-stage ViewNode consensus is out of scope; StorageNode registration and expansion are in scope; runtime Raft Add/Remove/Promote is reserved.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Verified existing capabilities affected by this feature are identified and excluded from unnecessary replanning.
  - PASS: existing Raft metadata commit/snapshot/recovery and StorageNode chunk durability are preserved as protected baseline.
- Any protocol, public API, or persisted format change is either absent or explicitly justified with migration and regression coverage.
  - PASS with scoped risk: existing `raft.proto`, `metadata.proto`, `storage_node.proto` semantics are preserved. A new ViewNode discovery contract and config schema are planned as additive surfaces. Any future proto field additions require contract tests and caller updates.
- Durability, crash-recovery, and restart-recovery implications are stated for every affected path in `node`, `replication`, `storage`, or `state_machine`.
  - PASS: Raft quorum is unchanged; StorageNode chunk publish semantics and node.identity durability are first-class tasks; orphan/staging cleanup is tested.
- Linux-specific validation is explicitly labeled, and Windows/macOS fallback, adaptation, or deferred follow-up is recorded.
  - PASS: Linux runs full E2E/failure matrix; Windows gets config/startup/path/durability smoke and explicit fallback contract.
- Test entry points are defined through CTest plus any justified platform-specific script or preset additions.
  - PASS: new tests are planned in `tests/` and wired through `tests/CMakeLists.txt`; quickstart uses existing presets and `test.sh`.
- Observability and diagnostics impact is captured for high-risk work.
  - PASS: ViewNode status, leader hints, request_id, node_id, placement decision reasons, checksum failures, and local task reports are planned.

## Project Structure

### Documentation (this feature)

```text
specs/008-integrated-object-storage-system/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── risk-register.md
├── checklists/
│   └── requirements.md
├── contracts/
│   ├── app-cli.md
│   ├── cluster-config.md
│   ├── metadata-object-flow.md
│   ├── storage-node-flow.md
│   └── view-node-discovery.md
├── task-reports/
└── tasks.md
```

### Source Code (repository root)

```text
apps/
├── main.cpp                         # existing raft_demo entry; do not expand into new product logic
├── raft_metadata_client.cpp          # existing metadata-only client; preserve boundary
├── view_node_app.cpp                 # planned thin ViewNode entry
├── metadata_node_app.cpp             # planned thin Raft MetadataNode entry
├── storage_node_app.cpp              # planned thin StorageNode entry
├── storage_client.cpp                # planned upload/download client
└── storage_bench.cpp                 # optional benchmark entry

proto/
├── common.proto                      # existing shared metadata messages; preserve field semantics
├── raft.proto                        # existing Raft RPC; no object payload
├── metadata.proto                    # existing metadata RPC; additive changes only if necessary
├── storage_node.proto                # existing StorageNode RPC; bounded chunk payload path
└── view.proto                        # planned additive ViewNode discovery/registration contract

modules/
├── cluster/
│   └── ...                           # planned config generation/loading and durable node.identity helpers
├── raft/
│   ├── common/                       # NodeConfig and metadata command boundaries
│   ├── metadata/                     # object/chunk/write-plan metadata records
│   ├── node/                         # RaftNode safety preserved; membership dynamic change reserved
│   ├── service/                      # metadata service adapter and leader hints
│   ├── storage/                      # Raft persistence protected; no payload ingress
│   └── state_machine/                # metadata object visibility and manifest queries
├── store/
│   ├── common/                       # StorageNode identity/checksum types
│   ├── chunk/                        # chunk store publish/read/delete
│   ├── io/                           # cross-platform durable file semantics
│   ├── node/                         # StorageNode service/client and registry facts
│   ├── placement/                    # healthy/capacity-based placement decisions
│   ├── upload/                       # upload coordinator, metadata/chunk adapters
│   ├── transfer/                     # planned storage_client upload/download orchestration
│   └── maintenance/                  # orphan/staging cleanup hooks
└── view/
    └── ...                           # planned ViewNode registry/discovery module with module-notes.md

tests/
├── integrated_object_storage_e2e_test.cpp
├── integrated_object_storage_quorum_test.cpp
├── integrated_object_storage_recovery_test.cpp
├── integrated_object_storage_concurrency_test.cpp
├── view_node_discovery_test.cpp
├── node_identity_test.cpp
└── support/
    └── integrated_cluster_test_utils.h
```

**Structure Decision**: Use the existing CQUPT_Raft layout and `raft_core` target as the shared library boundary. New app targets stay thin. New ViewNode logic belongs in a separate module because ViewNode has a real product boundary distinct from Raft metadata and StorageNode registry. Unified config and identity helpers belong in `modules/cluster` so app startup can share durable identity and topology parsing without coupling Raft and StorageNode internals. Existing `modules/store/placement` and `modules/store/upload` are extended rather than duplicated, while download/client transfer orchestration lives in `modules/store/transfer` to keep app code thin.

## Phase 0: Research

Research output is captured in [research.md](research.md). All design choices with meaningful ambiguity have concrete decisions:

- ViewNode is service discovery and observability only, not a consistency authority.
- Raft quorum remains based on committed membership voter count.
- Initial Raft membership is generated by config; runtime membership changes are future work.
- StorageNode registration and placement eligibility are dynamic.
- node.identity is durable local state and must not be regenerated on restart.
- Upload/download are real file flows with per-chunk and whole-object checksums.
- Large object handling must avoid full-file buffering; current bounded chunk RPC can be used for first stage while streaming RPC remains a future optimization if contract changes are approved.

## Phase 1: Design & Contracts

Design artifacts:

- [data-model.md](data-model.md): ClusterConfig, NodeIdentity, NodeRegistration, ViewNodeRegistry, RaftMembership, WritePlan, Placement, ObjectManifest, ChunkManifest, ObjectTransferSession, cleanup states.
- [contracts/view-node-discovery.md](contracts/view-node-discovery.md): ViewNode registration, heartbeat, discovery, leader observation, and membership boundary.
- [contracts/metadata-object-flow.md](contracts/metadata-object-flow.md): Create/plan, commit, head/list manifest, quorum failure, idempotency, payload boundary.
- [contracts/storage-node-flow.md](contracts/storage-node-flow.md): write/read/delete chunk, checksum, publish durability, restart recovery, bounded payload.
- [contracts/cluster-config.md](contracts/cluster-config.md): unified config schema and config generation rules.
- [contracts/app-cli.md](contracts/app-cli.md): app entrypoint and startup/client command contract.
- [quickstart.md](quickstart.md): build, config generation, cluster startup, upload/download, checksum and failure validation.

## Post-Design Constitution Check

- Preserve verified core: PASS. Tasks target integration surfaces, config, apps, ViewNode, identity, tests, and minimal adapters.
- Protocol/public API/persisted format: PASS with explicit additive surfaces. Existing proto semantics and persisted formats are protected. Additive ViewNode/config contracts require tests.
- Durability/recovery: PASS. node.identity, StorageNode publish, orphan cleanup, and Raft quorum tests are mandatory.
- Cross-platform: PASS. Linux full validation plus Windows startup/path/durability smoke and documented fallback.
- Observability/minimal surface: PASS. New ViewNode is justified by service discovery boundary; app entries remain thin; module-notes.md required for new modules.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| New `modules/view` module | ViewNode has distinct service discovery/observability responsibility and must not live inside Raft or StorageNode ownership | Reusing `modules/store/node/StorageNodeRegistry` would blur ViewNode scope and make it tempting to treat data-plane registry as cluster-wide authority |
| New `modules/cluster` module | Config generation/loading and durable node.identity are shared by ViewNode, MetadataNode, StorageNode, and client startup | Putting config/identity parsing in apps would duplicate validation and make identity durability inconsistent |
| New `modules/store/transfer` module | `storage_client` needs real file upload/download orchestration, stream checksum, discovery adapters, and manifest-driven reads while staying thin | Extending `apps/storage_client.cpp` with all orchestration would violate app boundary; putting download logic in `upload/` would blur module responsibility |
| Additive `proto/view.proto` contract | Client and nodes need a stable discovery/registration surface independent of metadata and storage chunk RPC | Encoding ViewNode calls into existing metadata/storage protos would violate proto boundary rules and couple discovery to object commit or chunk data |
| New app targets | User explicitly requires independent ViewNode, MetadataNode, StorageNode, and client apps | Expanding `raft_demo` would produce another fixed-topology demo and make config-driven industrialization harder |

## Implementation Boundaries

- Do not put object payload, complete file bytes, or chunk payload into Raft commands, Raft logs, Raft snapshots, metadata snapshots, or task reports.
- Do not change Raft election or commit quorum semantics to improve availability.
- Do not count ViewNode-registered Raft nodes as voters unless Raft has committed that membership.
- Do not move metadata commit logic into StorageNode or ViewNode.
- Do not move chunk IO or publish durability into Raft metadata modules.
- Do not make apps own business logic; apps parse config, construct services/clients, and delegate.
- Do not update high-frequency docs with execution logs; use `specs/008-integrated-object-storage-system/task-reports/` for later implementation notes.
