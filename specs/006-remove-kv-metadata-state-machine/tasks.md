# Tasks: Remove KV Metadata State Machine

**Input**: 设计文档位于 `specs/006-remove-kv-metadata-state-machine/`  
**Prerequisites**: `plan.md`、`spec.md`，以及已生成的 `research.md`、`data-model.md`、`contracts/`、`quickstart.md`

**Tests**: 本特性明确要求测试驱动迁移。每个用户故事都包含必须先补齐或先迁移的 CMake/CTest 任务。  

**Organization**: 任务按用户故事组织，但由于本特性是“删除旧主路径并替换为新主路径”的破坏性迁移，执行顺序采用严格的 `US1 → US2 → US3 → US4` 渐进路线，而不是并行删改 KV。

## Format: `[ID] [P?] [Story] Description`

- **[P]**: 可并行执行，且与同阶段未完成任务不存在文件冲突
- **[Story]**: 对应 `spec.md` 的用户故事标签
- 每个任务都必须包含明确文件路径

## Path Conventions

- 源码模块位于 `modules/raft/`
- 协议定义位于 `proto/`
- 入口程序位于 `apps/`
- 测试位于 `tests/`
- feature 文档与任务报告位于 `specs/006-remove-kv-metadata-state-machine/`

## Task Report Contract

- 每个任务完成后，必须在 `specs/006-remove-kv-metadata-state-machine/task-reports/` 下写同编号报告，例如 `T014-node-wiring.md`
- 每份任务报告至少包含：
  - Linux 结果
  - Windows 结果
  - CTest 结果
  - KV removal status
  - 与并发 / 跨平台 / durability / recovery 相关的影响说明（如适用）
- 不把执行日志追加到 `README.md`、`AGENTS.md`、`spec.md`、`plan.md`、`tasks.md`

## Phase 1: Setup（勘察与任务落地准备）

**Purpose**: 先把 KV 依赖面、节点装配面、CTest 入口面摸清楚，并建立任务报告约束

- [x] T001 盘点 `modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp`、`apps/main.cpp` 中 `RaftNode` 默认装配与 `KvStateMachine` / `MetadataStateMachine` 注入关系，并记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T001-node-wiring-audit.md`
- [x] T002 盘点 `proto/raft.proto`、`CMakeLists.txt`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1`、`apps/raft_kv_client.cpp` 中 KV service/target/script 依赖，并记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T002-kv-surface-audit.md`
- [x] T003 建立 `specs/006-remove-kv-metadata-state-machine/task-reports/` 的任务报告命名规范与跨平台验证命令清单，并记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T003-reporting-contract.md`

---

## Phase 2: Foundational（所有用户故事的阻塞前置）

**Purpose**: 建立 metadata-only 迁移所需的共享类型、共享测试辅助与跨平台验证骨架

**⚠️ CRITICAL**: 本阶段未完成前，不允许删除 KV 代码或翻转默认业务装配

- [x] T004 [P] 扩展 `modules/raft/common/metadata_command.h`、`modules/raft/common/metadata_command.cpp`、`modules/raft/common/metadata_result.h` 的 V2 类型骨架，预留 `BucketRecord`、`ObjectRecord`、`ChunkRef`、`RequestRecord`、`TombstoneRecord`、`CreateBucket`、`DeleteBucket`、`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject`
- [x] T005 [P] 将 `modules/raft/common/config.h` 与 `modules/raft/common/propose.h` 中的共享配置升级为 metadata-only admission/backpressure/timeout 配置骨架，替代仅面向 KV 的请求限制表达
- [x] T006 [P] 新增 `tests/metadata_test_utils.h`、`tests/metadata_test_utils.cpp`，提供 bucket/object request builder、metadata cluster fixture、跨平台数据目录 helper、metadata 断言 helper
- [x] T007 更新 `proto/raft.proto`，为 metadata-only 业务 RPC 与非-KV 管理面 RPC 预留协议骨架，同时保留 `RaftService` 不变
- [x] T008 在 `tests/metadata_node_wiring_test.cpp` 与 `tests/CMakeLists.txt` 中建立 metadata-only 默认装配回归测试骨架，保证后续翻转 `RaftNode` 主路径时有明确失败信号
- [x] T009 [P] 新增 `tests/no_kv_surface_audit.cmake` 并在 `tests/CMakeLists.txt` 接入跨平台审计骨架，用于最终验证主构建/主测试路径不再依赖 KV target、KV service、KV state machine

**Checkpoint**: metadata-only 迁移骨架到位，可以开始实现用户故事

---

## Phase 3: User Story 1 - 让系统只保留元数据主路径（Priority: P1） 🎯

**Goal**: 让默认节点装配、默认服务装配、默认 CMake/CTest/脚本入口都切到 metadata-only 主路径，不再把 KV 作为受支持主路径

**Independent Test**: 默认 `RaftNode` 实例化后只暴露 metadata 业务路径；Linux/Windows 的主构建与主 CTest 入口不再以 `KvService`、`KvStateMachineTest`、`raft_kv_client` 作为受支持路径

### Tests for User Story 1

- [x] T010 [P] [US1] 在 `tests/node_admin_service_test.cpp` 中补齐不依赖 `KvService` 的 `Status` / `Health` / `Metrics` 管理面回归测试
- [x] T011 [P] [US1] 在 `tests/metadata_main_path_test.cpp` 中补齐默认 `RaftNode` 仅通过 `MetadataService` 跑通单节点与三节点 smoke 的回归测试

### Implementation for User Story 1

- [x] T012 [US1] 从 `modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp` 中移除 `CompositeKvMetadataStateMachine`，并把默认 `RaftNode` 构造切换为 metadata-only 业务状态机装配
- [x] T013 [US1] 将 `modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp` 中 `Describe()`、`DebugGetValue()`、KV 相关 `RpcKind` 与状态机 dynamic_cast 调试路径改为 metadata-oriented 主路径
- [x] T014 [US1] 新增 `modules/raft/service/node_admin_service_impl.h`、`modules/raft/service/node_admin_service_impl.cpp`，并在 `modules/raft/node/raft_node.cpp` 中注册 non-KV 管理面服务替代 `KvService` 上的状态接口
- [x] T015 [US1] 更新 `apps/main.cpp` 与 `CMakeLists.txt`，让 `raft_demo` 启动时只暴露 `RaftService`、`MetadataService` 与 non-KV 管理面服务，并让 `raft_metadata_client` 成为唯一业务 CLI target
- [x] T016 [US1] 更新 `tests/CMakeLists.txt`、`test.sh`、`test.ps1`，删除 `kv-service` / `KvStateMachineTest` / `raft_kv_client` 的主入口宣传与默认执行路径，改为 metadata-only 主验证入口
- [x] T017 [US1] 运行 US1 相关 Linux/Windows 构建与 CTest 验证，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T017-us1-main-path-validation.md`

**Checkpoint**: 默认业务主路径已经 metadata-only，但此时允许仓库里仍暂存未删除的 KV 残留文件，只要它们不再是主构建/主测试/主运行路径

---

## Phase 4: User Story 2 - 管理对象元数据生命周期（Priority: P2）

**Goal**: 用 bucket/object 语义替换当前 record-centric metadata V1，建立 metadata-only 的正式业务模型与客户端路径

**Independent Test**: 仅通过 metadata CLI 和 metadata service 就能完成 `CreateBucket`、`DeleteBucket`、`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject`、`HeadObject`、`ListObjects`，且查询不依赖日志扫描

### Tests for User Story 2

- [x] T018 [P] [US2] 扩展 `tests/metadata_command_test.cpp`，覆盖 bucket/object/abort/delete 的 V2 序列化、反序列化、fingerprint 与非法输入约束
- [x] T019 [P] [US2] 重写 `tests/metadata_state_machine_test.cpp`，覆盖 bucket 生命周期、对象 `PENDING/COMMITTED/DELETED` 状态转换、abort 语义、request_id 幂等与 stale retry 冲突
- [x] T020 [P] [US2] 新增 `tests/metadata_service_e2e_test.cpp`，覆盖 `CreateBucket`、`DeleteBucket`、`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject`、`HeadObject`、`ListObjects` 的端到端契约

### Implementation for User Story 2

- [x] T021 [US2] 重构 `modules/raft/common/metadata_command.h`、`modules/raft/common/metadata_command.cpp`、`modules/raft/common/metadata_result.h` 为 bucket/object/request/tombstone V2 模型与状态码
- [x] T022 [US2] 重构 `modules/raft/state_machine/metadata_state_machine.h`、`modules/raft/state_machine/metadata_state_machine.cpp`，引入 `bucket_table`、`object_table`、`object_index`、`chunk_ref_table`、`request_table`、`tombstone_table`
- [x] T023 [US2] 更新 `proto/raft.proto`，把当前 record-centric `CreateMetadataRecord` / `CommitMetadataRecord` / `DeleteMetadataRecord` / `HeadMetadataRecord` / `ListMetadataRecords` 升级为 bucket/object V2 RPC
- [x] T024 [US2] 更新 `modules/raft/service/metadata_service_impl.h`、`modules/raft/service/metadata_service_impl.cpp`，让 MetadataService 按 bucket/object 语义提案 `MetadataCommand` 并返回显式冲突/重放结果
- [x] T025 [US2] 扩展 `apps/raft_metadata_client.cpp`，实现 `create-bucket`、`delete-bucket`、`create-object`、`commit-object`、`abort-object`、`delete-object`、`head-object`、`list-objects`
- [x] T026 [US2] 更新 `tests/metadata_client_scenario_test.cpp` 与 `tests/CMakeLists.txt`，将 client scenario 从 V1 record 流程切换为 bucket/object 主路径
- [x] T027 [US2] 运行 US2 相关 Linux/Windows 构建与 CTest 验证，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T027-us2-lifecycle-validation.md`

**Checkpoint**: metadata-only 主路径已经具备正式的 bucket/object 生命周期能力，可替代旧 KV demo 的业务闭环

---

## Phase 5: User Story 3 - 在并发、重试和切主下保持正确结果（Priority: P3）

**Goal**: 把 metadata-only 主路径从“功能可用”强化为“高并发工业级控制面可用”，保证顺序 apply、并发读路径、幂等、背压与 leader switch 正确

**Independent Test**: 多客户端并发 `CreateObject` / `CommitObject` / `DeleteObject` / `HeadObject` / `ListObjects` / duplicate `request_id` / leader switch 下，既不 double apply，也不暴露半更新状态

### Tests for User Story 3

- [x] T028 [P] [US3] 新增 `tests/metadata_state_machine_concurrency_test.cpp`，覆盖 `shared_mutex` 读写分离、duplicate request_id 并发、Head/List 不读到半更新状态
- [x] T029 [P] [US3] 新增 `tests/metadata_concurrency_stress_test.cpp`，覆盖多客户端并发 create/commit/delete/head/list、bounded queue、timeout、backpressure 行为
- [x] T030 [P] [US3] 将 `tests/test_t017_leader_switch_ordering.cpp`、`tests/test_raft_commit_apply.cpp`、`tests/test_raft_log_replication.cpp` 从 KV 断言迁移为 metadata ordered-apply / leader-switch 断言

### Implementation for User Story 3

- [x] T031 [US3] 将 `modules/raft/state_machine/metadata_state_machine.h`、`modules/raft/state_machine/metadata_state_machine.cpp` 升级为 `shared_mutex` 并发模型，保证 apply 单写、Head/List 并发读、request/object/tombstone 原子更新
- [x] T032 [US3] 在 `modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp`、`modules/raft/common/propose.h` 中补齐 metadata proposal admission、bounded queue/backpressure、timeout handling、no-double-apply 防护
- [x] T033 [US3] 更新 `modules/raft/common/metadata_result.h`、`proto/raft.proto`、`modules/raft/service/metadata_service_impl.cpp`、`apps/raft_metadata_client.cpp`，为 overload、timeout、retry、idempotency conflict 提供稳定对外语义
- [x] T034 [US3] 将 `tests/raft_integration_test.cpp`、`tests/metadata_failover_test.cpp`、`tests/metadata_main_path_test.cpp` 的集群断言全部改为 metadata 读路径与 metadata 状态一致性断言
- [x] T035 [US3] 运行 US3 相关 Linux/Windows 构建与 CTest 验证，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T035-us3-concurrency-validation.md`

**Checkpoint**: metadata-only 主路径已经满足并发、幂等、leader switch 和 read/write 分离要求

---

## Phase 6: User Story 4 - 在恢复和追赶后保持元数据一致（Priority: P4）

**Goal**: 保留并迁移 Raft 的高价值恢复、快照、日志回放、follower catch-up 与 restart recovery 能力，让它们全部落在 metadata-only 主路径上

**Independent Test**: snapshot、restart recovery、follower catch-up、state machine replay、leader failover 都通过 metadata-only 断言验证，且 Linux/Windows 都有构建与 CTest 结果

### Tests for User Story 4

- [x] T036 [P] [US4] 扩展 `tests/metadata_snapshot_test.cpp`，覆盖 snapshot V2 头、`last_applied_index`/`term`、bucket/object/index/request/tombstone 恢复与 checksum 校验
- [x] T037 [P] [US4] 将 `tests/snapshot_test.cpp`、`tests/persistence_test.cpp`、`tests/test_raft_snapshot_catchup.cpp`、`tests/test_raft_snapshot_restart.cpp` 从 KV 断言迁移为 metadata snapshot/save/load/replay/catch-up 断言
- [x] T038 [P] [US4] 新增 `tests/metadata_recovery_stress_test.cpp`，覆盖并发 apply/query 期间 snapshot、并发写入后的 restart recovery、catch-up 后元数据一致性

### Implementation for User Story 4

- [x] T039 [US4] 升级 `modules/raft/state_machine/metadata_state_machine.cpp` 的 snapshot 数据文件格式为 V2，纳入 `last_applied_index`、`last_applied_term`、buckets、objects、object_index、chunk_refs、request_table、tombstones
- [x] T040 [US4] 更新 `modules/raft/node/raft_node.cpp` 的 snapshot worker、startup load、post-snapshot replay 逻辑，确保 `LoadSnapshot + Replay` 只回放 `index > last_applied_index`
- [x] T041 [US4] 保持 `modules/raft/storage/snapshot_storage.h`、`modules/raft/storage/snapshot_storage.cpp` 的 staging publish / checksum / fsync / FlushFileBuffers 语义不减弱，同时补齐 metadata-only 边界诊断
- [x] T042 [US4] 将 `tests/test_raft_split_brain.cpp`、`tests/test_raft_snapshot_diagnosis.cpp`、`tests/persistence_more_test.cpp`、`tests/test_raft_replicator_behavior.cpp`、`tests/test_raft_segment_storage.cpp` 的 KV 断言全部替换为 metadata 状态或 metadata 查询断言
- [x] T043 [US4] 运行 US4 相关 Linux/Windows 构建与 CTest 验证，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T043-us4-recovery-validation.md`

**Checkpoint**: 所有高价值 Raft 恢复/追赶/重启能力都已迁移到 metadata-only 主路径

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: 彻底删除 KV 残留、补齐 no-KV 审计与跨平台最终验收

- [x] T044 [P] 从 `modules/raft/common/command.h`、`modules/raft/common/command.cpp`、`tests/test_command.cpp` 中删除 KV `SET/DEL` 命令路径，只保留 metadata-only 业务载荷与 Raft 内部必需标记
- [x] T045 [P] 删除 `modules/raft/state_machine/state_machine.h`、`modules/raft/state_machine/state_machine.cpp`，并退役 `tests/test_state_machine.cpp`
- [x] T046 [P] 删除 `modules/raft/service/kv_service_impl.h`、`modules/raft/service/kv_service_impl.cpp`，并退役 `tests/test_kv_service.cpp`
- [x] T047 [P] 删除 `apps/raft_kv_client.cpp` 并从 `CMakeLists.txt` 中移除 `raft_kv_client` target
- [x] T048 [P] 从 `proto/raft.proto` 中彻底移除 `KvService`、`KvStatusCode`、`Put/Get/Delete` 相关 message/RPC，并确保 `raft_proto` 的唯一业务 RPC 面是 metadata-only
- [x] T049 [P] 清理 `docs/CURRENT_INDUSTRIALIZATION_ANALYSIS.md`、`docs/PERSISTENCE_DURABILITY_CONTRACT.md`、`tests/README.md`、`README.md` 中的 KV 主路径描述，改为 metadata-only 主路径描述
- [X] T050 在 `tests/no_kv_surface_audit.cmake`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1` 中接入最终 no-KV 审计，确保主构建/主 CTest 路径检测到残留 KV 符号、KV target 或 KV regression-only path 时直接失败
- [x] T051 运行 Linux 全量 configure/build/CTest 并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T051-linux-final-validation.md`
- [x] T052 运行 Windows 全量 configure/build/CTest 并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T052-windows-final-validation.md`
- [x] T053 汇总 KV 删除完成度、回归迁移完成度、并发/恢复/跨平台风险余项，并记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T053-kv-removal-summary.md`
- [x] T054 逐条执行并校正 `specs/006-remove-kv-metadata-state-machine/quickstart.md` 中的主验证命令，确保 quickstart 与最终 metadata-only 主路径一致

---

## Phase 8: Final KV Residual Cleanup & Test Deduplication

**Purpose**: 在 metadata-only 主路径已完成的前提下，继续收口 KV 物理删除、测试去重与严格 no-KV 审计，避免历史兼容残留长期留在生产与测试主路径

**Guardrails**:

- 本阶段是 `T044/T045` blocked 后的后续收尾，不得把历史 blocker 伪装成已完成。
- 本阶段每个任务都必须写任务报告，路径位于 `specs/006-remove-kv-metadata-state-machine/task-reports/`。
- 本阶段每份报告必须至少包含：
  - Linux 结果
  - Windows 结果
  - CTest 结果
  - KV residual status
  - 是否降低覆盖
  - 是否删除生产 KV 残留
  - 剩余风险
- 若 `T056/T057` 发现新的阻塞项或删改风险，必须同步更新 `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`，不得伪造成完成。

- [x] T055 全面审计 `modules/`、`apps/`、`proto/`、`tests/`、`CMakeLists.txt`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1`、`docs/` 中 `kv` / `KV` / `Kv` / `kSet` / `kDelete` / `CommandType::kSet` / `CommandType::kDelete` / `KvStateMachine` / `state_machine.h` / `state_machine.cpp` 等残留，并在 `specs/006-remove-kv-metadata-state-machine/task-reports/T055-kv-residual-audit.md` 中分类为“必须删除的生产残留”“必须迁移或删除的测试残留”“仅允许存在于历史 task report / migration 文档的说明性残留”“no-KV audit 自身允许出现的检测关键词”；本任务只产出审计报告，不修改源码
- [x] T056 [P] 基于 `specs/006-remove-kv-metadata-state-machine/task-reports/T055-kv-residual-audit.md` 清理 `modules/raft/common/`、`modules/raft/node/`、`modules/raft/state_machine/`、`modules/raft/service/`、`apps/`、`proto/` 与根级 `CMakeLists.txt` 中的生产 KV 残留，包括 `CommandType::kSet` / `CommandType::kDelete`、旧 KV command codec、`KvStateMachine`、`state_machine.h/.cpp`、dead include、dead log message、旧 KV target/source 残留，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T056-production-kv-residual-cleanup.md`
- [x] T057 [P] 基于 `specs/006-remove-kv-metadata-state-machine/task-reports/T055-kv-residual-audit.md` 审计并清理 `tests/`、`tests/support/`、`tests/CMakeLists.txt` 中重复测试文件、重复 case、重复 helper 与旧 KV 语义测试，重点处理 `tests/test_state_machine.cpp`、`tests/support/raft_snapshot_restart_test_utils.h`、snapshot/restart/recovery/catch-up 相关重复 helper、旧 `SetCommand` / `DeleteCommand` 测试，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T057-test-dedup-and-legacy-kv-cleanup.md`
- [x] T058 基于 `T055`、`T056`、`T057` 的结果强化 `tests/no_kv_surface_audit.cmake`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1` 的 no-KV 审计策略，让生产代码、测试主路径、CMake target/source、脚本入口中的禁止 KV surface 直接失败，同时允许历史 `task-reports/`、migration 文档与 audit 自身保留检测关键词，并将结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T058-strict-no-kv-surface-audit.md`
- [ ] T059 基于 `T058` 校正 `test.sh`、`test.ps1` 与相关 no-KV 分组入口，使 `./test.sh --skip-configure --skip-build --group no-kv` 成为真正轻量的 no-KV 审计命令，只执行 direct `NoKvSurfaceAudit` 与必要 metadata-only smoke，不默认展开为过重的全量 CTest，并将 Linux/Windows 复验结果记录到 `specs/006-remove-kv-metadata-state-machine/task-reports/T059-final-no-kv-audit-validation.md`

**Checkpoint**: 完成 `T055–T059` 后，才能重新判断“KV 物理删除是否完成”“no-KV audit 是否可升格为严格 fail gate”“测试主路径是否已完成去重与历史兼容收口”

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1: Setup** 无依赖，可立即开始
- **Phase 2: Foundational** 依赖 Phase 1，且阻塞所有用户故事
- **Phase 3: US1** 依赖 Phase 2；必须先完成默认 metadata-only 主路径翻转
- **Phase 4: US2** 依赖 US1；因为 bucket/object 生命周期要建立在新的默认 metadata-only 主路径上
- **Phase 5: US3** 依赖 US2；因为并发、背压、leader switch 强化必须作用于最终生命周期模型
- **Phase 6: US4** 依赖 US3；因为恢复、快照、catch-up 要验证的是最终并发强化后的 metadata-only 主路径
- **Phase 7: Polish** 依赖所有用户故事完成；此时再做物理删除与跨平台最终验收
- **Phase 8: Final KV Residual Cleanup & Test Deduplication** 依赖 Phase 7 的主路径收口结果；只在 metadata-only 主路径、KV service/client/proto 退役、Linux/Windows 最终验证口径明确后推进

### User Story Dependencies

- **US1**: 无其他故事依赖，但必须在删 KV 文件前先完成 metadata-only 主路径翻转
- **US2**: 依赖 US1 的默认装配和验证入口迁移
- **US3**: 依赖 US2 的 bucket/object/request/tombstone 正式模型
- **US4**: 依赖 US3 的 ordered apply、并发读写和稳定对外语义

### Parallel Opportunities

- `T004`、`T005`、`T006`、`T009` 可并行
- `T010`、`T011` 可并行
- `T018`、`T019`、`T020` 可并行
- `T028`、`T029`、`T030` 可并行
- `T036`、`T037`、`T038` 可并行
- `T044`、`T045`、`T046`、`T047`、`T048`、`T049` 可在最终删除窗口内并行，但必须在 `T050` 之前完成
- `T056`、`T057` 可并行，但都必须在 `T055` 完成后开始；`T058` 依赖 `T056` 与 `T057`，`T059` 依赖 `T058`

---

## Parallel Example: User Story 1

```bash
# 并行补齐 metadata-only 主路径测试：
T010 tests/node_admin_service_test.cpp
T011 tests/metadata_main_path_test.cpp

# 在测试骨架稳定后串行翻转默认装配，再更新入口脚本：
T012 -> T013 -> T014 -> T015 -> T016 -> T017
```

## Parallel Example: User Story 2

```bash
# 并行补齐 bucket/object 生命周期测试：
T018 tests/metadata_command_test.cpp
T019 tests/metadata_state_machine_test.cpp
T020 tests/metadata_service_e2e_test.cpp

# 然后按协议 -> 状态机 -> 服务 -> CLI -> 场景验证顺序推进：
T021 -> T022 -> T023 -> T024 -> T025 -> T026 -> T027
```

## Parallel Example: User Story 3

```bash
# 并行补齐并发与切主测试：
T028 tests/metadata_state_machine_concurrency_test.cpp
T029 tests/metadata_concurrency_stress_test.cpp
T030 tests/test_t017_leader_switch_ordering.cpp tests/test_raft_commit_apply.cpp tests/test_raft_log_replication.cpp

# 然后串行落地并发控制与对外语义：
T031 -> T032 -> T033 -> T034 -> T035
```

## Parallel Example: User Story 4

```bash
# 并行补齐恢复与快照测试：
T036 tests/metadata_snapshot_test.cpp
T037 tests/snapshot_test.cpp tests/persistence_test.cpp tests/test_raft_snapshot_catchup.cpp tests/test_raft_snapshot_restart.cpp
T038 tests/metadata_recovery_stress_test.cpp

# 然后串行升级 snapshot V2 与 replay 边界：
T039 -> T040 -> T041 -> T042 -> T043
```

## Parallel Example: Phase 8

```bash
# 先完成残留分类审计：
T055 specs/006-remove-kv-metadata-state-machine/task-reports/T055-kv-residual-audit.md

# 然后并行推进生产代码残留清理与测试去重：
T056 modules/raft/common/command.h modules/raft/common/command.cpp modules/raft/node/raft_node.cpp modules/raft/state_machine/state_machine.h modules/raft/state_machine/state_machine.cpp
T057 tests/test_state_machine.cpp tests/support/raft_snapshot_restart_test_utils.h tests/CMakeLists.txt

# 最后再串行强化 no-KV 审计并收口脚本入口：
T058 -> T059
```

---

## Implementation Strategy

### Recommended MVP

这个特性的推荐 MVP 不是单独的 US1，而是：

1. 完成 Phase 1 与 Phase 2
2. 完成 US1，把默认主路径切到 metadata-only
3. 立即完成 US2，让 bucket/object 生命周期成为唯一正式业务模型
4. 停下来做一次 Linux/Windows 双平台 CTest 验证

原因：仅完成 US1 只能证明“KV 不再是主路径”，但还不能证明“metadata-only 主路径已经具备正式业务闭环”。

### Incremental Delivery

1. **Foundation**: 完成勘察、共享类型、共享测试辅助与 no-KV 审计骨架
2. **Main Path Flip**: 完成 US1，确保默认装配与脚本入口只认 metadata-only
3. **Business Model Completion**: 完成 US2，补齐 bucket/object/request/tombstone 主模型
4. **Concurrency Hardening**: 完成 US3，证明工业级并发语义成立
5. **Recovery Hardening**: 完成 US4，证明 snapshot/restart/catch-up/leader switch 不被削弱
6. **Final Removal**: 在最终删除窗口中物理移除 KV 残留，并做 Linux/Windows 全量验收

### Stop Conditions

- US1 完成后：如果默认主路径仍能走 KV，则不得进入 US2
- US2 完成后：如果 `HeadObject` / `ListObjects` 仍依赖日志扫描或旧 record-centric V1 路径，则不得进入 US3
- US3 完成后：如果仍存在 unordered apply、double apply、半更新可见性或无界 admission，则不得进入 US4
- US4 完成后：如果任何必须保留的恢复/catch-up 回归仍依赖 KV 断言，则不得进入最终删除窗口

---

## Notes

- 所有新增测试和迁移后的旧测试都必须以 CMake/CTest 为主验证方式
- 不允许新增任何新的 KV fallback、KV compatibility mode 或 KV regression-only path
- 不允许把“单线程通过”当成高并发最终验收
- 不允许只做 Linux 验证而跳过 Windows，或只做 Windows 验证而跳过 Linux
- 任何触及 snapshot/restart/catch-up 的任务都必须显式说明 durability / replay / cross-platform 影响

## 恢复测试补充硬约束

- 恢复测试不能只验证对象可见性。任何 snapshot recovery、restart recovery、log replay、follower catch-up、concurrent writes 后恢复类测试，都不能只检查 `HeadObject` / `ListObjects` 是否能看到对象，也不能只检查 `ObjectRecord` 是否存在；必须验证完整元数据一致性。
- `request_table` 幂等事实必须被验证：`request_table` 必须在 snapshot save/load/restart 后保留；重复 `request_id` 在恢复后必须仍然保持 deduplicated；restart 后 duplicate `request_id` 不得触发 duplicate apply；follower catch-up 后 `request_table` 必须与 leader 保持一致。
- `tombstone` 删除事实必须被验证：已删除对象在恢复后不得重新出现；`tombstone` 状态必须穿过 snapshot save/load/restart；snapshot 前已 apply 的 `DeleteObject` 在恢复后必须仍为 deleted；snapshot 后经 replay 的 `DeleteObject` 在恢复后也必须仍为 deleted。
- `object_table` 与 `object_index` 一致性必须被验证：两者必须 mutually consistent；`object_index` 不得丢失 committed object mapping；`object_index` 不得保留无效 deleted-object mapping；`object_index` 不得指向缺失的 `ObjectRecord`；除非对象被显式 tombstoned，否则 `ObjectRecord` 不得脱离有效 index state 单独存在；`HeadObject` / `ListObjects` 返回结果必须与 `object_table` / `object_index` / `tombstone` 事实一致。
- `last_applied_index` / `last_applied_term` 边界一致性必须被验证：`snapshot.meta.last_applied_index` 与 metadata snapshot 内容必须一致；`snapshot.meta.last_applied_term` 与 metadata snapshot 内容必须一致；`MetadataStateMachine` 内部 applied boundary 必须与 snapshot metadata 一致；replay 必须严格从 `last_applied_index` 之后开始；replay 不得跳过已 committed 的 `MetadataCommand`；replay 不得重新 apply 已包含在 snapshot 中的 entry。
- 恢复后明确禁止出现以下问题：restart 后 duplicate apply、restart 后丢失 committed `MetadataCommand`、deleted object reappear、重复 `request_id` deduplication failure、stale `ObjectRecord`、stale `object_index`、不一致的 `ObjectRecord`、不一致的 snapshot boundary、follower catch-up metadata divergence。
- follower catch-up 恢复一致性测试不能只验证对象可见性，还必须验证 `request_table` 一致性、`tombstone` 一致性、`object_table` / `object_index` 一致性、`last_applied_index` 一致性、`last_applied_term` 一致性；并且 concurrent writes 后 follower catch-up 的元数据事实必须与 leader 完全一致。
- concurrent recovery 场景必须验证完整元数据一致性：concurrent writes 后的 restart recovery 不能只看对象是否可见；concurrent apply/query 期间触发的 snapshot 必须产出一致的 recovery image；crash 前并发重复 `request_id` 在 restart 后必须仍保持 deduplicated；crash 前并发 `DeleteObject` 不得在恢复后造成 deleted object resurrection；crash 前并发 `CommitObject` 不得在恢复后丢失 committed object metadata。
