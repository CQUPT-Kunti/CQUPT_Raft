# Tasks: 007-storage-node-data-plane

**Input**: Design documents from `specs/007-storage-node-data-plane/`  
**Prerequisites**: [spec.md](./spec.md), [plan.md](./plan.md), [research.md](./research.md), [data-model.md](./data-model.md), [quickstart.md](./quickstart.md), [cross-task-risk-notes.md](./cross-task-risk-notes.md), [contracts/](./contracts/)  
**Generated**: 2026-05-25  
**Scope**: 在现有 CQUPT_Raft metadata-only Raft control-plane 上新增 StorageNode chunk/data-plane。不是新建项目，不重写 Raft，不恢复旧 KV。

## Current Background

当前仓库已有 `MetadataStateMachine`、`MetadataService`、`RaftNode::ProposeMetadata()`、`ObjectRecord.chunks`、`ChunkRef.replica_nodes/checksum`、metadata snapshot V2 和 no-KV audit。CodeGraph 复核显示当前生产模块中没有 `StorageNode`、`PlacementManager`、`RepairManager`、`RebalanceManager`、`ScrubManager`、`WriteChunk`、`ReadChunk` 或 `DeleteChunk` 数据面实现。因此 007 的任务必须先补本地 data-plane 底座，再逐步接入服务、上传/读取/删除闭环和后续工业化后台能力。

## Hard Constraints

- Raft 只负责 metadata/control-plane。
- StorageNode 负责 chunk/data-plane。
- `MetadataStateMachine` 不保存 chunk 字节。
- Raft log 不写 object payload。
- StorageNode 不决定 object 是否 committed。
- metadata 是 object state 和 chunk manifest 的 source of truth。
- 上传流程必须是 metadata pending -> chunk durable -> metadata commit。
- 读取流程必须是 read metadata first -> read StorageNode replicas。
- 删除流程必须是 metadata tombstone/DELETED -> background async chunk cleanup。
- 不得重新引入 `CommandType::kSet`、`CommandType::kDelete`、`KvStateMachine`、`KvService`、`raft_kv_client`、`DebugGetValue`、KV proto、KV target、KV fallback、KV regression-only path。
- tests 中不得使用 `SetCommand` / `DeleteCommand` / KV 状态机断言。
- 不得把 StorageNode 设计成 KV，不得把对象数据写入 Raft log，不得回滚或绕过 006 no-KV 收口成果。

## Phase Overview

1. Setup: 建立 StorageNode 模块、CMake/test 入口和 guardrail。
2. Foundational: ChunkStore、LocalDiskChunkStore、ChunkIndex、durable file、checksum、bounded executor。
3. US1: 上传对象并在 chunk durable 后提交 metadata。
4. US2: 读取 committed 对象并校验 chunk。
5. US3: 删除对象并异步清理 chunk。
6. US4: StorageNode heartbeat / registry / health-aware placement。
7. US5: StorageNode 重启恢复、staging cleanup、corrupted quarantine、跨平台 crash 矩阵。
8. US6: Scrub、Repair、Rebalance 后段工业化能力。
9. Final: no-KV audit、跨平台矩阵、全量验证和收口。

## Phase 1: Setup

**Purpose**: 建立可编译的 StorageNode 数据面模块边界和测试入口。此阶段可以修改构建系统，但执行本 tasks 阶段不做实现。

- [x] T001 创建 `modules/store/common/store_types.h` 和 `modules/store/common/store_types.cpp` 的最小类型占位并接入 `CMakeLists.txt` 的 `raft_core`；允许修改：`modules/store/common/store_types.h`、`modules/store/common/store_types.cpp`、`CMakeLists.txt`；验收：`cmake --build --preset debug-ninja-low-parallel` 能编译空模块；依赖：无
- [x] T002 创建 Store common 基础测试目标 `tests/store_types_test.cpp` 并在 `tests/CMakeLists.txt` 增加 `storage-node;platform-neutral` 标签；允许修改：`tests/store_types_test.cpp`、`tests/CMakeLists.txt`；验收：`ctest --test-dir build/linux -R "store_types" --output-on-failure` PASS；依赖：T001
- [x] T003 在 `tests/no_kv_surface_audit.cmake` 中确认新 `modules/store/`、`proto/storage_node.proto` 和 storage tests 被 no-KV scan 覆盖；允许修改：`tests/no_kv_surface_audit.cmake`；验收：`cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit` PASS；依赖：T001
- [x] T004 为 StorageNode CTest 标签预留 `storage-node`、`storage-node-concurrency`、`storage-node-recovery`、`storage-node-cross-platform` 分组；允许修改：`tests/CMakeLists.txt`；验收：`ctest -N -L storage-node` 能列出 StorageNode 相关测试入口；依赖：T002
- [x] T005 [P] 创建测试辅助目录 `tests/support/store_test_utils.h` 用于临时数据目录、chunk payload 和 checksum fixture；允许修改：`tests/support/store_test_utils.h`；验收：后续 storage tests 可复用且不读取构建产物/运行数据作为源码；依赖：T002
- [x] T006 运行 no-KV 基线审计并记录失败摘要或 PASS；允许修改：无生产文件，仅运行 `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`；验收：PASS 或失败摘要不含新增 StorageNode KV 违规；依赖：T003

## Phase 2: Foundational

**Purpose**: 所有用户故事的阻塞前置。完成前不要做 StorageNodeService、上传闭环、Repair 或 Rebalance。

- [x] T007 定义 `StorageNodeStatusCode`、`ChunkState`、`ChunkChecksum`、`ChunkIdentity`、`ChunkMetadata`、`ChunkIndexEntry`、`ChunkReplica` 基础类型；允许修改：`modules/store/common/store_types.h`、`modules/store/common/store_types.cpp`；验收：`tests/store_types_test.cpp` 覆盖状态枚举、错误分类和默认值；依赖：T001
- [x] T008 [P] 为 `chunk_id = object_id + version + chunk_index` 规则实现生成和校验 helper；允许修改：`modules/store/common/store_types.h`、`modules/store/common/store_types.cpp`、`tests/store_types_test.cpp`；验收：非法 object_id、version、chunk_index、路径逃逸均返回明确错误；依赖：T007
- [x] T009 [P] 实现 checksum helper，支持 write/read/scrub/repair/migration 复用；允许修改：`modules/store/common/store_types.h`、`modules/store/common/store_types.cpp`、`tests/store_types_test.cpp`；验收：相同 payload checksum 稳定、mismatch 可识别、不做内容寻址或全局去重；依赖：T007
- [x] T010 定义 `ChunkStore` 抽象接口覆盖 `WriteChunk`、`ReadChunk`、`DeleteChunk`、`StatChunk`、`ListChunks`；允许修改：`modules/store/chunk/chunk_store.h`、`modules/store/chunk/chunk_store.cpp`、`modules/store/chunk/module-notes.md`、`CMakeLists.txt`；验收：接口不暴露 Raft 类型，不保存 object payload 到 metadata；依赖：T007
- [x] T011 定义 durable file 抽象接口和共享错误映射；允许修改：`modules/store/io/durable_file.h`、`modules/store/io/durable_file.cpp`、`modules/store/io/module-notes.md`、`CMakeLists.txt`；验收：接口表达 file flush、atomic publish、directory sync、path normalization、unsupported/error 分类；依赖：T007
- [x] T012 [P] 为 durable file 写跨平台契约测试骨架；允许修改：`tests/store_durable_file_test.cpp`、`tests/CMakeLists.txt`；验收：测试覆盖 required operation 不能 no-op success；依赖：T011
- [x] T013 实现 Linux durable file 路径：`fsync`/`fdatasync`、same-filesystem rename、parent directory sync 和 POSIX error mapping；允许修改：`modules/store/io/durable_file.cpp`、`tests/store_durable_file_test.cpp`；验收：Linux 上 disk full/permission denied/IO error 分类明确，flush/publish/directory sync 失败不返回成功；依赖：T011、T012
- [x] T014 实现 Windows durable file 路径：`FlushFileBuffers`、Windows file handle、`MoveFileEx`/`ReplaceFile` 风险边界、long path/UTF-8 path 错误分类；允许修改：`modules/store/io/durable_file.cpp`、`tests/store_durable_file_test.cpp`；验收：Windows 分支无 no-op success，无法等价 directory sync 时返回 explicit unsupported 或记录较弱 contract；依赖：T011、T012
- [ ] T014-WIN 在 Windows 环境验证 `WindowsDurableFile` 分支编译和 `store_durable_file` 测试通过；允许修改：`modules/store/io/durable_file.cpp`、`tests/store_durable_file_test.cpp`；验收：Windows build/test 实机通过并补充分支验证报告；依赖：T014
- [x] T015 实现路径 normalization 和 chunk 目录布局 helper；允许修改：`modules/store/io/durable_file.h`、`modules/store/io/durable_file.cpp`、`tests/store_durable_file_test.cpp`；验收：拒绝绝对路径逃逸、`..`、非法 chunk id、Windows reserved names；依赖：T011
- [x] T016 定义 `ChunkIndex` 接口和 sharded map 结构；允许修改：`modules/store/index/chunk_index.h`、`modules/store/index/chunk_index.cpp`、`CMakeLists.txt`；验收：支持 insert/update/find/list/remove，状态区分 live/staging/deleting/deleted/quarantined/corrupted/missing；依赖：T007
- [x] T017 [P] 为 `ChunkIndex` 写单元测试；允许修改：`tests/store_chunk_index_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖 sharded list、pagination、state filter、duplicate update、missing lookup；依赖：T016
- [x] T018 实现 per-chunk lock 和 lock striping；允许修改：`modules/store/index/chunk_index.h`、`modules/store/index/chunk_index.cpp`、`tests/store_chunk_index_test.cpp`；验收：并发同一 chunk write/delete 串行，不同 chunk 可并行；依赖：T016、T017
- [x] T019 定义 bounded executor / bounded IO queue；允许修改：`modules/store/runtime/storage_executor.h`、`modules/store/runtime/storage_executor.cpp`、`CMakeLists.txt`；验收：队列容量、timeout、cancellation、overloaded result 可测；依赖：T007
- [x] T020 [P] 为 bounded executor 写单元测试；允许修改：`tests/store_executor_test.cpp`、`tests/CMakeLists.txt`；验收：队列满返回 overloaded，取消任务不泄漏线程；依赖：T019
- [x] T021 定义 `LocalDiskChunkStore` 构造配置和目录初始化；允许修改：`modules/store/chunk/local_disk_chunk_store.h`、`modules/store/chunk/local_disk_chunk_store.cpp`、`CMakeLists.txt`；验收：无效 data_dir、权限错误、目录创建失败均返回明确错误；依赖：T010、T011、T016
- [x] T022 [P] 为 `LocalDiskChunkStore` 写基础单元测试；允许修改：`tests/local_disk_chunk_store_test.cpp`、`tests/CMakeLists.txt`；验收：临时目录隔离，测试不依赖固定绝对路径；依赖：T021
- [x] T023 实现 `LocalDiskChunkStore::WriteChunk` 的 staging -> checksum -> flush -> publish -> index 更新路径；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/local_disk_chunk_store_test.cpp`；验收：checksum on write、same chunk same content idempotent、mismatch conflict、publish 前不可读；依赖：T013、T014、T015、T018、T021
- [ ] T023-WIN 在 Windows 环境验证 `LocalDiskChunkStore::WriteChunk` 与 `WindowsDurableFile` 的 staging/publish/directory-sync contract；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/local_disk_chunk_store_test.cpp`、`modules/store/io/durable_file.cpp`；验收：Windows build/test 实机通过，或明确记录 current unsupported/failure contract 并补充分支验证报告；依赖：T023、T014-WIN
- [x] T024 实现 `LocalDiskChunkStore::ReadChunk` 和 checksum on read；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/local_disk_chunk_store_test.cpp`；验收：只读 live chunk，corrupted/quarantined/staging/deleting 不返回成功，checksum mismatch 标记 corrupted；依赖：T023
- [x] T025 实现 `DeleteChunk`、`StatChunk`、`ListChunks` 的本地语义；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/local_disk_chunk_store_test.cpp`；验收：重复 delete 幂等，Stat/List 区分 live/staging/deleting/deleted/quarantined/corrupted/missing；依赖：T024
- [ ] T025-WIN 在 Windows 环境验证 `LocalDiskChunkStore::DeleteChunk` 的 sharing violation / remove semantics 与 `StatChunk` / `ListChunks` 路径行为；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/local_disk_chunk_store_test.cpp`；验收：Windows build/test 实机通过，或明确记录 current delete/stat/list contract 与必要修正；依赖：T025、T023-WIN
- [x] T026 添加本地高并发 chunk IO 压力测试；允许修改：`tests/store_concurrency_stress_test.cpp`、`tests/CMakeLists.txt`；验收：至少 100 并发 chunk 操作无数据竞争、无无界队列增长，CTest 标签为 `storage-node-concurrency`；依赖：T018、T020、T025
- [ ] T026-WIN 在 Windows 环境验证 `LocalDiskChunkStore` 高并发 write/read/delete/stat/list 的 sharing violation、open-handle delete 和 durable publish 后读取 contract；允许修改：`tests/store_concurrency_stress_test.cpp`、`modules/store/chunk/local_disk_chunk_store.cpp`、`modules/store/io/durable_file.cpp`；验收：Windows build/test 实机通过，或明确记录当前并发 contract 与必要修正；依赖：T026、T023-WIN、T025-WIN

## Phase 3: User Story 1 - 上传对象并在数据 durable 后提交元数据 (Priority: P1) MVP

**Goal**: 完成最小上传闭环：`CreateObject` pending -> chunk durable -> `CommitObject` manifest -> committed 可见。  
**Independent Test**: 上传时 commit 前 `HeadObject` 不可见；每个 chunk 满足最小成功副本数后 `CommitObject` 写入 `ChunkRef.replica_nodes` 并可见。

### Tests for User Story 1

- [x] T027 [P] [US1] 添加上传闭环集成测试骨架；允许修改：`tests/storage_upload_integration_test.cpp`、`tests/CMakeLists.txt`；验收：测试先表达 commit 前不可见和 durable 后可见；依赖：T025
- [x] T028 [P] [US1] 添加 `WriteChunk` contract 测试；允许修改：`tests/storage_write_chunk_contract_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖 request_id、checksum、durable、already_exists、conflict、timeout/overloaded 映射；依赖：T025

### Implementation for User Story 1

- [x] T029 [US1] 将 `contracts/storage-node.proto.draft` 转为真实 `proto/storage_node.proto` 的 MVP 写入接口；允许修改：`proto/storage_node.proto`、`CMakeLists.txt`；验收：只新增 StorageNode proto，不修改 `raft.proto`/`metadata.proto` 语义，不出现 KV message；依赖：T028
- [x] T030 [US1] 为 `storage_node.proto` 增加生成目标和链接边界；允许修改：`CMakeLists.txt`、`tests/CMakeLists.txt`；验收：新增 proto target 不让 `raft_proto` 依赖 storage 生成代码，构建通过；依赖：T029
- [x] T031 [US1] 实现 `StorageNodeService::WriteChunk` 适配层；允许修改：`modules/store/node/storage_node_service.h`、`modules/store/node/storage_node_service.cpp`、`modules/store/node/module-notes.md`、`modules/store/node/AGENTS.md`、`tests/storage_node_service_test.cpp`、`tests/CMakeLists.txt`、`CMakeLists.txt`；验收：service 只调用 `ChunkStore`，不调用 `RaftNode::ProposeMetadata()`，不决定 object commit；依赖：T030、T023
- [x] T032 [US1] 实现 `StorageNodeClient::WriteChunk` deadline/retry/cancellation/error mapping；允许修改：`modules/store/node/storage_node_client.h`、`modules/store/node/storage_node_client.cpp`、`modules/store/node/module-notes.md`、`modules/store/node/AGENTS.md`、`tests/storage_node_client_test.cpp`、`tests/CMakeLists.txt`、`CMakeLists.txt`；验收：retry 只用于 retryable 错误，checksum mismatch/conflict 不重试；依赖：T031
- [x] T033 [P] [US1] 实现默认 `ReplicaPolicy` 的 3 副本/最小 2 成功规则；允许修改：`modules/store/placement/replica_policy.h`、`modules/store/placement/replica_policy.cpp`、`modules/store/placement/module-notes.md`、`modules/store/placement/AGENTS.md`、`modules/store/AGENTS.md`、`tests/store_placement_policy_test.cpp`、`tests/CMakeLists.txt`、`CMakeLists.txt`；验收：不引入 erasure coding，策略可配置但默认值固定为 3/2；依赖：T007
- [x] T034 [US1] 实现 MVP `PlacementManager` 的静态候选节点选择；允许修改：`modules/store/placement/placement_manager.h`、`modules/store/placement/placement_manager.cpp`、`tests/store_placement_manager_test.cpp`；验收：能选择 `replica_count` 个节点，跳过显式 excluded nodes，输出 decision reasons；依赖：T033
- [x] T035 [US1] 实现上传集成 helper，用 metadata client/coordinator 调用 `CreateObject`、`WriteChunk`、`CommitObject`；允许修改：`modules/store/upload/upload_coordinator.h`、`modules/store/upload/upload_coordinator.cpp`、`modules/store/upload/module-notes.md`、`modules/store/upload/AGENTS.md`、`tests/support/storage_upload_test_utils.h`、`tests/storage_upload_coordinator_test.cpp`；验收：helper 不新增 gateway 生产角色，不让 StorageNode 调用 `CommitObject`；依赖：T032、T034
- [x] T036 [US1] 在上传闭环中生成 `ChunkRef` manifest 并写入成功 durable replicas；允许修改：`tests/support/storage_upload_test_utils.h`、`tests/storage_upload_integration_test.cpp`；验收：`CommitObjectRequest.chunks` 的 `chunk_id/offset/size/checksum/replica_nodes` 与本地 durable facts 一致；依赖：T035
- [x] T037 [US1] 覆盖部分副本写失败时不提交对象并触发 AbortObject 或 cleanup candidate；允许修改：`modules/store/upload/upload_coordinator.h`、`modules/store/upload/upload_coordinator.cpp`、`modules/store/upload/module-notes.md`、`tests/storage_upload_coordinator_test.cpp`、`tests/storage_upload_integration_test.cpp`；验收：最小 2 成功不满足时 `HeadObject` 不可见，已写 chunk 标记为待 GC；依赖：T036
- [x] T038 [US1] 增加上传路径 no-KV 回归检查；允许修改：`tests/no_kv_surface_audit.cmake`、`tests/storage_upload_integration_test.cpp`；验收：上传测试不使用 `SetCommand`、`DeleteCommand`、`DebugGetValue` 或 KV 断言；依赖：T037
- [x] T039 [US1] 运行 US1 验证命令；允许修改：无生产文件，仅运行 `ctest -R "storage_upload|storage_node_service|storage_placement|NoKvSurfaceAudit" --output-on-failure`；验收：PASS；依赖：T027-T038

## Phase 4: User Story 2 - 读取 committed 对象并校验 chunk (Priority: P2)

**Goal**: 读取路径先查 metadata，再按 manifest 读取 StorageNode replicas，并在 read 上校验 checksum。  
**Independent Test**: 预置 committed object manifest，读取时按 offset 组装，首选副本失败可 fallback，checksum mismatch 不返回损坏数据。

### Tests for User Story 2

- [x] T040 [P] [US2] 添加 committed manifest 读取集成测试；允许修改：`tests/storage_read_integration_test.cpp`、`tests/CMakeLists.txt`；验收：测试覆盖 metadata lookup first、offset ordering、未 committed/deleted 不读 StorageNode；依赖：T039
- [x] T041 [P] [US2] 添加 `ReadChunk` contract 测试；允许修改：`tests/storage_read_chunk_contract_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖 full/range read、checksum verify、corrupted/quarantined/deleted/staging 拒绝、not_found/invalid_argument/timeout/unavailable/io_error 映射、只读取 live chunk、不决定 object committed 可见性；依赖：T031

### Implementation for User Story 2

- [x] T042 [US2] 在 `proto/storage_node.proto` 中补齐 `ReadChunk` MVP 字段和生成映射；允许修改：`proto/storage_node.proto`、`CMakeLists.txt`；验收：不修改 metadata proto，read response 不携带 object state 决策；依赖：T030
- [x] T043 [US2] 实现 `StorageNodeService::ReadChunk`；允许修改：`modules/store/node/storage_node_service.cpp`、`modules/store/node/storage_node_service.h`；验收：只从 `ChunkStore` 读取 live chunk，checksum mismatch 返回明确错误并标记 corrupted；依赖：T024、T042
- [x] T044 [US2] 实现 `StorageNodeClient::ReadChunk` 输入/输出结构；允许修改：`modules/store/node/storage_node_client.cpp`、`modules/store/node/storage_node_client.h`；验收：本地请求/响应与 T042/T043 `ReadChunk` proto/service 语义一致，timeout/cancel/unavailable/io/invalid argument 映射明确，不实现 read replica fallback；依赖：T043
- [X] T045 [US2] 实现 read replica selection / fallback 的最小 committed-manifest 读路径；实际修改：`modules/store/placement/replica_policy.cpp`、`modules/store/placement/replica_policy.h`、`modules/store/node/storage_node_client.cpp`、`modules/store/node/storage_node_client.h`、`tests/store_placement_policy_test.cpp`、`tests/storage_read_integration_test.cpp`；验收：跳过 corrupted/unavailable/stale/overloaded candidates，并按已排序副本执行 fallback；依赖：T033、T044
- [X] T046 [US2] 实现测试侧 ReadObject by manifest helper；实际修改：`tests/support/storage_read_test_utils.h`、`tests/storage_read_integration_test.cpp`；验收：helper 先调用 `HeadObject`，只读取 COMMITTED manifest，按 offset 拼接，并复用 T045 的 replica selection / fallback 语义；依赖：T040、T045
- [X] T047 [US2] 覆盖副本失败 fallback 和 checksum mismatch quarantine；实际修改：`tests/storage_read_integration_test.cpp`、`tests/local_disk_chunk_store_test.cpp`；验收：首选副本 unavailable/not_found/timeout/checksum mismatch 时读备用副本，known corrupted 副本不会被选为有效读取源，所有副本失败返回明确错误；依赖：T046
- [X] T048 [US2] 运行 US2 验证命令；允许修改：无生产文件，仅运行 `ctest -R "storage_read|storage_node_service|local_disk_chunk_store" --output-on-failure`；验收：PASS；依赖：T040-T047

## Phase 5: User Story 3 - 删除对象并异步清理 chunk (Priority: P3)

**Goal**: 删除路径先提交 metadata tombstone/DELETED，再由 metadata-driven GC 异步删除 chunk。  
**Independent Test**: 删除 committed 对象后立即不可见，后台 GC 只删除不再属于 committed live manifest 的 chunk，重复删除和重启后继续清理均幂等。

### Tests for User Story 3

- [X] T049 [P] [US3] 添加删除闭环和 GC safety 测试；允许修改：`tests/storage_delete_gc_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖 `DeleteObject -> invisible -> async chunk cleanup` 和 live manifest 保护；依赖：T048
- [X] T050 [P] [US3] 添加 `DeleteChunk` / `BatchDeleteChunks` contract 测试；允许修改：`tests/storage_delete_chunk_contract_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖 missing/deleted 幂等、partial batch result、retryable failures；依赖：T031

### Implementation for User Story 3

- [x] T051 [US3] 在 `proto/storage_node.proto` 中补齐 `DeleteChunk`、`BatchDeleteChunks` proto schema 并接入现有 `storage_node_proto` codegen；允许修改：`proto/storage_node.proto`、`tests/storage_node_client_test.cpp`、`CMakeLists.txt`；验收：proto 仍独立于 `metadata.proto`，删除 RPC 只表达 chunk data-plane 删除，不恢复 KV RPC；依赖：T050
- [x] T052 [US3] 实现 `StorageNodeService::DeleteChunk` 和 `BatchDeleteChunks`；允许修改：`modules/store/node/storage_node_service.cpp`、`modules/store/node/storage_node_service.h`、`modules/store/node/module-notes.md`、`tests/storage_node_service_test.cpp`；验收：删除幂等、identity/checksum mismatch 不误删、partial batch result 与 retry 分类在 service 层保持可观察；依赖：T050、T051
- [x] T053 [US3] 实现 `StorageNodeClient` 删除和批量删除调用；允许修改：`modules/store/node/storage_node_client.cpp`、`modules/store/node/storage_node_client.h`；验收：partial batch result 可重试，非 retryable 错误不盲重试；依赖：T052
- [x] T054 [US3] 实现 `GarbageCollector` 任务模型和 bounded 后台队列；允许修改：`modules/store/maintenance/garbage_collector.h`、`modules/store/maintenance/garbage_collector.cpp`、`CMakeLists.txt`；验收：GC task 有 reason、metadata_boundary、attempts、last_error、state；依赖：T019、T053
- [x] T055 [US3] 实现 metadata-driven GC safety check；允许修改：`modules/store/maintenance/garbage_collector.cpp`、`tests/storage_delete_gc_test.cpp`；验收：任何 committed live manifest 引用的 chunk 不会被 GC 删除；依赖：T054
- [x] T056 [US3] 覆盖 pending timeout、failed upload cleanup、AbortObject cleanup 的 GC candidate 生成；允许修改：`modules/store/maintenance/garbage_collector.h`、`modules/store/maintenance/garbage_collector.cpp`、`modules/store/maintenance/module-notes.md`、`tests/storage_garbage_collector_test.cpp`、`tests/storage_delete_gc_test.cpp`、`tests/storage_upload_coordinator_test.cpp`；验收：未 committed 的 durable orphan 可清理，已 committed live chunk 受保护；依赖：T055
- [x] T057 [US3] 覆盖 StorageNode 删除过程中重启后的继续清理；允许修改：`modules/store/maintenance/garbage_collector.h`、`modules/store/maintenance/garbage_collector.cpp`、`modules/store/maintenance/gc_task_store.h`、`modules/store/maintenance/gc_task_store.cpp`、`modules/store/maintenance/module-notes.md`、`modules/store/maintenance/AGENTS.md`、`tests/storage_garbage_collector_test.cpp`、`CMakeLists.txt`；验收：重复 delete、missing、deleting 状态均幂等；依赖：T056
- [x] T058 [US3] 运行 US3 低并发验证命令；允许修改：无生产文件，仅运行 `CTEST_PARALLEL_LEVEL=1 ctest -R "storage_delete_gc|storage_node_service" --output-on-failure`；验收：PASS；依赖：T049-T057

## Phase 6: User Story 4 - 查看 StorageNode 容量、健康和负载 (Priority: P4)

**Goal**: StorageNode 上报 capacity/health/load，registry 可供 Placement、读副本选择、Repair/Rebalance 使用。  
**Independent Test**: 模拟多个 StorageNode 心跳，验证 capacity、used、available、chunk_count、health、disk pressure、IO error、load 和 liveness 可被决策消费。

### Tests for User Story 4

- [ ] T059 [P] [US4] 添加 heartbeat/registry 单元测试；允许修改：`tests/storage_heartbeat_registry_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖注册、重复注册、stale heartbeat、node liveness；依赖：T058
- [ ] T060 [P] [US4] 添加 health-aware placement 测试；允许修改：`tests/storage_placement_test.cpp`；验收：unhealthy/overloaded/disk-pressure/insufficient-capacity 节点被跳过或降权；依赖：T034

### Implementation for User Story 4

- [ ] T061 [US4] 在 `proto/storage_node.proto` 中补齐 `ReportHealth`、`ReportCapacity`、`ReportLoad`、`RegisterStorageNode`、`UpdateStorageNodeHeartbeat` 字段；允许修改：`proto/storage_node.proto`、`CMakeLists.txt`；验收：heartbeat 独立于 Raft heartbeat，不修改 `raft.proto`；依赖：T030
- [ ] T062 [US4] 定义 `StorageNodeHeartbeat`、`StorageNodeRegistry` 和 liveness 状态；允许修改：`modules/store/node/storage_node_registry.h`、`modules/store/node/storage_node_registry.cpp`、`CMakeLists.txt`；验收：字段包含 node_id、capacity、used、available、chunk_count、health、disk_pressure、io_error_count、load、last_seen、node_liveness；依赖：T061
- [ ] T063 [US4] 实现 `StorageNodeService` heartbeat/report/register 入口；允许修改：`modules/store/node/storage_node_service.cpp`、`modules/store/node/storage_node_service.h`；验收：同 sequence 幂等，stale heartbeat 不覆盖新 facts；依赖：T062
- [ ] T064 [US4] 实现 `StorageNodeClient` heartbeat/report/register 调用；允许修改：`modules/store/node/storage_node_client.cpp`、`modules/store/node/storage_node_client.h`；验收：control-plane unavailable/timeout 可重试，duplicate sequence 安全；依赖：T063
- [ ] T065 [US4] 将 registry facts 接入 `PlacementManager` eligibility；允许修改：`modules/store/placement/placement_manager.cpp`、`modules/store/placement/placement_manager.h`；验收：Placement 使用 capacity、health、load、disk pressure、failure-domain placeholder、hotspot signal；依赖：T060、T062
- [ ] T066 [US4] 将 registry facts 接入 read replica selection；允许修改：`modules/store/placement/replica_policy.cpp`、`modules/store/placement/replica_policy.h`；验收：read selection 降权 stale/unhealthy/overloaded replica；依赖：T045、T062
- [ ] T067 [US4] 运行 US4 验证命令；允许修改：无生产文件，仅运行 `ctest -R "storage_heartbeat_registry|storage_placement" --output-on-failure`；验收：PASS；依赖：T059-T066

## Phase 7: User Story 5 - StorageNode 重启后恢复本地 chunk 状态 (Priority: P5)

**Goal**: StorageNode 重启扫描本地 chunk 目录，重建 ChunkIndex，清理 stale staging，识别 partial/corrupted/orphan。  
**Independent Test**: 在 staging write、flush、rename、parent directory sync、delete 边界注入崩溃，重启后不暴露半有效 chunk。

### Tests for User Story 5

- [ ] T068 [P] [US5] 添加 restart index rebuild 测试；允许修改：`tests/store_recovery_test.cpp`、`tests/CMakeLists.txt`；验收：final live chunk 重建进 index，staging 不进入 live；依赖：T025
- [ ] T069 [P] [US5] 添加跨平台 durability 矩阵测试；允许修改：`tests/storage_cross_platform_durability_test.cpp`、`tests/CMakeLists.txt`；验收：覆盖 Linux fsync/fdatasync/directory sync 和 Windows FlushFileBuffers/MoveFileEx/ReplaceFile contract；依赖：T013、T014

### Implementation for User Story 5

- [ ] T070 [US5] 实现 `LocalDiskChunkStore::RebuildIndexFromDisk`；允许修改：`modules/store/chunk/local_disk_chunk_store.h`、`modules/store/chunk/local_disk_chunk_store.cpp`；验收：扫描 final/staging/quarantine/deleting 并重建 ChunkIndex；依赖：T025
- [ ] T071 [US5] 实现 stale staging cleanup 和 partial write detection；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/store_recovery_test.cpp`；验收：incomplete staging 删除或隔离，size/checksum 不一致不进入 live index；依赖：T070
- [ ] T072 [US5] 实现 corrupted chunk quarantine；允许修改：`modules/store/chunk/local_disk_chunk_store.cpp`、`tests/store_recovery_test.cpp`；验收：扫描或读取 checksum mismatch 后进入 quarantine/corrupted，不作为健康副本返回；依赖：T071
- [ ] T073 [US5] 覆盖 crash before fsync 和 crash after fsync before rename；允许修改：`tests/store_recovery_test.cpp`、`tests/support/store_test_utils.h`；验收：低并发运行，重启后无 partial live chunk；依赖：T071
- [ ] T074 [US5] 覆盖 crash after rename before parent directory sync 的平台 contract；允许修改：`tests/storage_cross_platform_durability_test.cpp`；验收：Linux 验证 directory sync，Windows 明确 supported/weaker/unsupported contract，不允许 no-op success；依赖：T069、T073
- [ ] T075 [US5] 覆盖 Windows long path、UTF-8 path、permission denied、disk full 错误分类；允许修改：`tests/storage_cross_platform_durability_test.cpp`、`modules/store/io/durable_file.cpp`；验收：Windows 和 Linux 分支错误分类一致，路径错误不静默成功；依赖：T014、T015
- [ ] T076 [US5] 覆盖 orphan chunk metadata-driven GC 边界；允许修改：`tests/store_recovery_test.cpp`、`tests/storage_delete_gc_test.cpp`；验收：本地 orphan 只有经 metadata live manifest 保护检查后才可删除；依赖：T055、T070
- [ ] T077 [US5] 运行 US5 低并发恢复验证；允许修改：无生产文件，仅运行 `CTEST_PARALLEL_LEVEL=1 ctest -R "store_recovery|storage_cross_platform_durability" --output-on-failure`；验收：PASS；依赖：T068-T076

## Phase 8: User Story 6 - 自动修复和再均衡 chunk 副本 (Priority: P6)

**Goal**: 在副本丢失、checksum mismatch、容量倾斜或新节点加入时，生成可重试、幂等、可观察的 scrub/repair/rebalance 任务。Rebalance 不进入 MVP 第一批实现任务。  
**Independent Test**: 模拟节点下线、副本损坏、容量倾斜和热点倾斜，验证 Scrub 先发现问题，Repair 再补齐副本，Rebalance 最后迁移且避免半迁移 manifest。

### Tests for User Story 6

- [ ] T078 [P] [US6] 添加 ScrubManager 测试；允许修改：`tests/storage_scrub_repair_test.cpp`、`tests/CMakeLists.txt`；验收：background checksum validation 能标记 corrupted replica 并输出 repair candidate；依赖：T077
- [ ] T079 [P] [US6] 添加 RepairManager 测试；允许修改：`tests/storage_scrub_repair_test.cpp`；验收：under-replicated chunk 选择 healthy source 和 healthy target，target durable 后才更新 facts；依赖：T078
- [ ] T080 [P] [US6] 添加 RebalanceManager 测试骨架；允许修改：`tests/storage_rebalance_test.cpp`、`tests/CMakeLists.txt`；验收：测试表达 target durable before manifest coordination 和 source cleanup after metadata update；依赖：T079

### Implementation for User Story 6

- [ ] T081 [US6] 在 `proto/storage_node.proto` 中补齐 `ScrubChunk` 和 `RepairChunk` RPC 字段；允许修改：`proto/storage_node.proto`、`CMakeLists.txt`；验收：RPC 只传 chunk bytes/facts，不传 object commit 决策；依赖：T061
- [ ] T082 [US6] 实现 `StorageNodeService::ScrubChunk` 和 `StorageNodeClient::ScrubChunk`；允许修改：`modules/store/node/storage_node_service.cpp`、`modules/store/node/storage_node_client.cpp`；验收：full checksum validation，mismatch 标记 corrupted/quarantine；依赖：T072、T081
- [ ] T083 [US6] 实现 `ScrubManager` bounded background queue；允许修改：`modules/store/maintenance/scrub_manager.h`、`modules/store/maintenance/scrub_manager.cpp`、`CMakeLists.txt`；验收：低优先级 IO，不饿死前台读写，输出 corrupted/lost/under-replicated facts；依赖：T082
- [ ] T084 [US6] 实现 `RepairManager` task model；允许修改：`modules/store/maintenance/repair_manager.h`、`modules/store/maintenance/repair_manager.cpp`、`CMakeLists.txt`；验收：task 记录 source_node、target_node、expected_checksum/size、state、progress、attempts、last_error；依赖：T083、T065
- [ ] T085 [US6] 实现 `RepairChunk` copy flow；允许修改：`modules/store/maintenance/repair_manager.cpp`、`modules/store/node/storage_node_client.cpp`、`tests/storage_scrub_repair_test.cpp`；验收：source checksum verified，target durable before metadata/replica health update，corrupted source 不可用；依赖：T084
- [ ] T086 [US6] 实现 under-replicated detection；允许修改：`modules/store/maintenance/repair_manager.cpp`、`modules/store/placement/replica_policy.cpp`、`tests/storage_scrub_repair_test.cpp`；验收：healthy replicas 低于 policy target 时生成 repair task，低于读安全阈值返回明确风险；依赖：T085
- [ ] T087 [US6] 实现 `RebalanceManager` task model 但不接入自动后台调度；允许修改：`modules/store/maintenance/rebalance_manager.h`、`modules/store/maintenance/rebalance_manager.cpp`、`CMakeLists.txt`；验收：capacity/hotspot/new node join task 可创建，默认不自动迁移 MVP 数据；依赖：T086
- [ ] T088 [US6] 实现 rebalance copy/verify/manifest coordination 占位流程；允许修改：`modules/store/maintenance/rebalance_manager.cpp`、`tests/storage_rebalance_test.cpp`；验收：target durable before manifest update，source cleanup after metadata no longer requires source，避免 half-migrated manifest；依赖：T087
- [ ] T089 [US6] 运行 US6 低并发验证；允许修改：无生产文件，仅运行 `CTEST_PARALLEL_LEVEL=1 ctest -R "storage_scrub_repair|storage_rebalance" --output-on-failure`；验收：PASS；依赖：T078-T088

## Final Phase: Polish And Cross-Cutting Validation

**Purpose**: 收口跨平台、no-KV、高并发和全量验证，不执行新的大范围功能。

- [ ] T090 运行 no-KV audit 并修复仅 007 引入的违规；允许修改：`tests/no_kv_surface_audit.cmake` 和触发违规的 007 文件；验收：`cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit` PASS；依赖：T089
- [ ] T091 运行 storage 高并发并行验证；允许修改：无生产文件，仅运行 `ctest -L storage-node-concurrency --output-on-failure`；验收：PASS，日志不含无界队列、deadlock、data race 摘要；依赖：T026、T089
- [ ] T092 运行 recovery/snapshot/catch-up 低并发回归；允许修改：无生产文件，仅运行 `CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence`；验收：PASS，不因 StorageNode 影响 Raft recovery/snapshot/catch-up；依赖：T077
- [ ] T093 运行全量平台中立回归；允许修改：无生产文件，仅运行 `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all`；验收：PASS 或按日志规则输出失败摘要；依赖：T090-T092
- [ ] T094 [P] 更新 future validation 说明；允许修改：`specs/007-storage-node-data-plane/quickstart.md`；验收：只记录真实实现后的验证命令，不宣称未实现能力已完成；依赖：T089
- [ ] T095 [P] 检查 `ObjectRecord.chunks` / `ChunkRef` 兼容性未被破坏；允许修改：`tests/metadata_manifest_test.cpp`、`tests/storage_upload_integration_test.cpp`；验收：metadata manifest 仍只保存 chunk refs，不保存 payload；依赖：T039、T048
- [ ] T096 生成最终实现报告摘要；允许修改：`specs/007-storage-node-data-plane/implementation-notes.md`；验收：记录已完成任务、跨平台 contract 偏差、no-KV audit 结果、剩余风险；依赖：T093

## Dependencies And Execution Order

### Phase Dependencies

- Phase 1 Setup 无依赖。
- Phase 2 Foundational 依赖 Phase 1，阻塞所有用户故事。
- US1 依赖 Phase 2，是 MVP。
- US2 依赖 US1 的 StorageNodeService/Client 和本地 store。
- US3 依赖 US1/US2 的 manifest、delete API 和 local store。
- US4 依赖 US1 的 Placement 基础和 service/client 边界。
- US5 依赖 Phase 2 的 durable file、ChunkIndex 和 US3 的 GC safety。
- US6 依赖 US4 heartbeat/registry、US5 recovery facts，并且 Scrub -> Repair -> Rebalance 顺序不可颠倒。
- Final phase 依赖目标用户故事完成情况。

### User Story Dependencies

- **US1 (P1)**: 必须先完成，提供最小可用 data-plane 上传闭环。
- **US2 (P2)**: 可在 US1 service/client 稳定后并行推进一部分 read tests，但完整验收依赖 US1 manifest。
- **US3 (P3)**: 依赖 US1/US2 的 committed object 和 read invisibility 语义。
- **US4 (P4)**: heartbeat/registry 可与 US3 部分并行，但 health-aware Placement 验收依赖 US1 Placement。
- **US5 (P5)**: recovery tests 必须低并发，依赖 durable file 和 LocalDiskChunkStore。
- **US6 (P6)**: 不进入 MVP 第一批；Scrub 早于 Repair，Repair 早于 Rebalance。

## Parallel Examples

### Foundational

```text
T012 durable file contract tests
T017 ChunkIndex tests
T020 bounded executor tests
```

### User Story 1

```text
T027 upload integration test skeleton
T028 WriteChunk contract test
T033 ReplicaPolicy default rule
```

### User Story 4

```text
T059 heartbeat/registry tests
T060 health-aware placement tests
```

## Test Matrix

| Area | Tests | Command Guidance | Concurrency |
|------|-------|------------------|-------------|
| LocalDiskChunkStore | `tests/local_disk_chunk_store_test.cpp` | `ctest -R local_disk_chunk_store --output-on-failure` | parallel ok |
| ChunkIndex | `tests/store_chunk_index_test.cpp` | `ctest -R store_chunk_index --output-on-failure` | parallel ok |
| Durable file | `tests/store_durable_file_test.cpp` | `ctest -R store_durable_file --output-on-failure` | platform-specific |
| Upload | `tests/storage_upload_integration_test.cpp` | `ctest -R storage_upload --output-on-failure` | low to moderate |
| Read | `tests/storage_read_integration_test.cpp` | `ctest -R storage_read --output-on-failure` | parallel reads ok |
| Delete/GC | `tests/storage_delete_gc_test.cpp` | `CTEST_PARALLEL_LEVEL=1 ctest -R storage_delete_gc --output-on-failure` | low concurrency |
| Heartbeat/Placement | `tests/storage_heartbeat_registry_test.cpp`, `tests/storage_placement_test.cpp` | `ctest -R "storage_heartbeat_registry|storage_placement" --output-on-failure` | parallel ok |
| Restart recovery | `tests/store_recovery_test.cpp` | `CTEST_PARALLEL_LEVEL=1 ctest -R store_recovery --output-on-failure` | low concurrency |
| Cross-platform durability | `tests/storage_cross_platform_durability_test.cpp` | platform-specific CTest filters | low concurrency |
| Concurrency stress | `tests/store_concurrency_stress_test.cpp` | `ctest -L storage-node-concurrency --output-on-failure` | parallel ok |
| Scrub/Repair/Rebalance | `tests/storage_scrub_repair_test.cpp`, `tests/storage_rebalance_test.cpp` | `CTEST_PARALLEL_LEVEL=1 ctest -R "storage_scrub_repair|storage_rebalance" --output-on-failure` | low concurrency |
| no-KV audit | `tests/no_kv_surface_audit.cmake` | `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit` | single |

## No-KV Audit Requirements

- 每个涉及 proto/CMake/service/test 的任务完成后，都要确认没有恢复 KV surface。
- StorageNode tests 禁止使用 `SetCommand`、`DeleteCommand`、`DebugGetValue` 或 KV 状态机断言。
- `proto/storage_node.proto` 不得复用旧 KV message 或 KV service 名称。
- `CMakeLists.txt` 不得恢复 `raft_kv_client` 或 KV-only target。
- 如果 no-KV audit 失败，只能修复新增的 StorageNode 违规，不能绕过审计。

## Cross-Platform Validation Requirements

- Linux required operations: `fsync` / `fdatasync`、parent directory sync、same-filesystem rename、disk full、permission denied、partial write、staging cleanup、atomic publish、checksum mismatch、restart index rebuild。
- Windows required operations: `FlushFileBuffers`、Windows file handle、`MoveFileEx` / `ReplaceFile`、Windows long path、UTF-8 path、disk full/access denied/sharing violation、staging cleanup、restart index rebuild。
- 任一平台无法提供等价 required durability operation 时，必须返回 explicit error 或记录较弱 contract；禁止 no-op success。
- storage 高并发测试可并行；recovery/snapshot/catch-up/crash 类测试低并发，必要时 `CTEST_PARALLEL_LEVEL=1`。

## Implementation Strategy

### MVP First

1. 完成 Phase 1 和 Phase 2。
2. 完成 US1 到 `CreateObject -> WriteChunk -> CommitObject` 闭环。
3. 运行 `ctest -R "storage_upload|storage_node_service|storage_placement|NoKvSurfaceAudit" --output-on-failure`。
4. 停下验证，不直接进入 Repair/Rebalance。

### Incremental Delivery

1. 本地 durable chunk store。
2. StorageNode service/client 写入能力。
3. 上传闭环。
4. 读取闭环。
5. 删除/GC。
6. heartbeat/health-aware placement。
7. restart recovery 和跨平台矩阵。
8. Scrub/Repair。
9. Rebalance。

## Next Step Recommendation

建议下一步从 T001 开始实施，但不要跳过 Phase 2 的 durable file、ChunkIndex 和 LocalDiskChunkStore。Rebalance 明确不属于 MVP 第一批实现任务。
