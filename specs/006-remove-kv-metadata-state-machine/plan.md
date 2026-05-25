# Implementation Plan: Remove KV Metadata State Machine

**Branch**: `007-remove-kv-metadata-state-machine` | **Date**: 2026-05-20 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `specs/006-remove-kv-metadata-state-machine/spec.md`

## Summary

本计划基于当前已存在的 C++20 Raft 工程做渐进式重构，而不是重写内核。现状是 `RaftNode` 默认仍装配 `CompositeKvMetadataStateMachine`，gRPC 服务同时注册 `KvService` 与 `MetadataService`，大部分 Raft 回归测试仍通过 `SET/DEL` 命令和 `DebugGetValue()` 校验 KV 状态；与此同时，工程已经具备一套 object-record 级别的 metadata V1 能力与若干 metadata 测试。  

`006` 的实现目标是把这套“双栈并存”状态收敛为 metadata-only 主路径：先完成节点装配和测试装配的 metadata 化，再扩展 metadata 模型到 bucket/object/request/tombstone/ordered-read-write 语义，随后迁移 proto/service/client/CTest，最后删除 KV command/state machine/service/client/CMake/test/script 入口，保证项目在构建、demo、回归验证和恢复路径上都不再依赖 KV。

## Technical Context

**Language/Version**: C++20  
当前仓库实际以根 [CMakeLists.txt](/home/yangjilei/Code/C++/CQUPT_Raft/CMakeLists.txt:21) 的 `CMAKE_CXX_STANDARD 20` 构建；本计划以仓库真实配置为准，而不是按“旧 C++17 项目”假设制定。  

**Primary Dependencies**: gRPC、Protobuf、GoogleTest、CMake、标准库、`std::filesystem`、线程/锁原语  

**Current Baseline**:
- `apps/main.cpp` 通过 `std::make_shared<RaftNode>(node_config, snapshot_config)` 实例化节点，[apps/main.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/apps/main.cpp:310)
- `RaftNode` 默认构造函数仍创建 `CompositeKvMetadataStateMachine`，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:134)
- `RaftNode::InitServer()` 同时注册 `RaftService`、`KvService`、`MetadataService`，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:388)
- 当前 metadata 实现已存在，但只覆盖 object-record 级 `create/commit/delete/head/list`，无 bucket、无 abort、无并发读写升级，[metadata_state_machine.h](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/state_machine/metadata_state_machine.h:34)
- 当前大多数 Raft 回归测试仍通过 `CommandType::kSet/kDelete` 提案并通过 `DebugGetValue()` 或 `Describe()` 中的 `kv=` 片段验证结果，例如 [test_raft_log_replication.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_log_replication.cpp:252)、[raft_integration_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/raft_integration_test.cpp:395)

**Storage**:
- Raft 核心持久化仍由 `FileRaftStorage` + `FileSnapshotStorage` 负责，目录级 snapshot publish 已具备 staging/rename/fsync/FlushFileBuffers 语义，[snapshot_storage.h](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/storage/snapshot_storage.h:40)
- metadata V1 状态机自己的 snapshot 数据文件当前只保存 `records_ / tombstones_ / replay_table_`，没有 bucket/object index、没有内部 `last_applied_index/term` 头，[metadata_state_machine.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/state_machine/metadata_state_machine.cpp:700)
- `RaftNode` snapshot worker 已在 `apply_mu_` 保护下调用 `state_machine_->SaveSnapshot()`，随后交给 `SnapshotStorage` 发布，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:2676)

**Testing**:
- 主体验证方式是 GoogleTest + CTest + `test.sh` / `test.ps1`
- Linux 分组脚本仍保留 `kv-service` 分组，[test.sh](/home/yangjilei/Code/C++/CQUPT_Raft/test.sh:43)
- Windows conservative fallback 仍把 `KvStateMachineTest` 作为保守入口的一部分，[test.ps1](/home/yangjilei/Code/C++/CQUPT_Raft/test.ps1:20)
- 已有 metadata 单测/快照/failover/client 场景测试可作为迁移起点：[metadata_state_machine_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/metadata_state_machine_test.cpp:40)、[metadata_snapshot_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/metadata_snapshot_test.cpp:90)、[metadata_failover_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/metadata_failover_test.cpp:75)、[metadata_client_scenario_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/metadata_client_scenario_test.cpp:143)

**Target Platform**: Linux 为主要验证环境；Windows 必须保持等价 durability / recovery / explicit-error 语义  

**Project Type**: 现有跨平台 Raft 一致性内核上的业务层替换与工业化迁移，不是 greenfield 系统  

**Performance Goals**:
- 支持多客户端并发 metadata 写/读请求
- 对 metadata 写入口提供 bounded admission / backpressure / timeout handling
- 保持单调有序 apply，不引入 out-of-order apply 或 double apply
- `HeadObject` / `ListObjects` 走并发读路径，不依赖扫描日志

**Constraints**:
- 最终主路径不得保留 `KvService`、`KvStateMachine`、KV `SET/DEL` 编码、`raft_kv_client`、KV-only CMake target、KV-only 回归入口
- 必须保留 Raft 核心语义、segment log、snapshot catalog、restart recovery、follower catch-up、cross-platform durability contract
- 不实现 DataNode、不实现真实对象数据面、不把真实对象数据写入 Raft 日志
- 根 `AGENTS.md` 禁止读取 `README.md`；因此本次 plan 只记录 README 清理需求，不对 README 现状做内容分析

**Scale/Scope**:
- 直接触达：`modules/raft/common`、`modules/raft/node`、`modules/raft/service`、`modules/raft/state_machine`、`proto`、`apps`、`tests`、`CMakeLists.txt`、`tests/CMakeLists.txt`、`test.sh`、`test.ps1`
- 间接验证：`modules/raft/storage`、`modules/raft/replication` 相关恢复/追赶路径

## Constitution Check

*GATE: Passed before Phase 0 research. Re-check after Phase 1 design.*

- Verified existing capabilities affected by this feature are identified and excluded from unnecessary replanning.  
  通过。选举、复制、提交、apply、SegmentLogStorage、SnapshotStorage、restart recovery、follower catch-up、state machine replay 都保留；本计划只替换业务模型和验证入口，不重写 Raft 核心。

- Any protocol, public API, or persisted format change is either absent or explicitly justified with migration and regression coverage.  
  通过，但有显式破坏性变更：`KvService` / KV client / KV command surface 将被删除；metadata RPC 将从 record-centric 扩展为 bucket/object-centric；metadata snapshot 数据文件会升级为 V2。兼容边界将通过版本化 loader、显式错误和全量测试迁移来约束，而不是保留 KV compatibility。

- Durability, crash-recovery, and restart-recovery implications are stated for every affected path in `node`, `replication`, `storage`, or `state_machine`.  
  通过。计划明确保留 `SnapshotStorage` 发布语义，在 metadata snapshot V2 中补足 `last_applied_index/term` 一致性、staging publish 与 replay boundary。

- Linux-specific validation is explicitly labeled, and Windows/macOS fallback, adaptation, or deferred follow-up is recorded.  
  通过。Linux 继续承担 durability / restart / catch-up 主验证；Windows 至少要有等价元数据语义和 CTest 入口迁移；不做静默降级。

- Test entry points are defined through CTest plus any justified platform-specific script or preset additions.  
  通过。计划明确迁移 `tests/CMakeLists.txt`、`test.sh`、`test.ps1` 的 KV 入口，并将 Raft 主回归映射到 metadata-only 路径。

- Observability and diagnostics impact is captured for high-risk work.  
  通过。当前 `Status/Health/Metrics` 绑在 `KvService` 上，本计划会把它们拆出为非-KV 管理面契约，保留现有诊断价值。

## Project Structure

### Documentation (this feature)

```text
specs/006-remove-kv-metadata-state-machine/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── metadata-service.md
│   └── metadata-client-cli.md
└── tasks.md                    # 由 /speckit-tasks 生成
```

### Source Code Scope For This Feature

```text
apps/
  main.cpp
  raft_kv_client.cpp                # final state: deleted
  raft_metadata_client.cpp

proto/
  raft.proto

modules/raft/common/
  command.h / command.cpp           # remove KV SET/DEL path and wrapper fallback
  metadata_command.h / .cpp         # extend to bucket/object lifecycle
  metadata_result.h
  config.h                          # replace kv_limits with metadata/admin limits

modules/raft/node/
  raft_node.h / raft_node.cpp       # metadata-only default assembly, apply/read hooks

modules/raft/service/
  kv_service_impl.h / .cpp          # final state: deleted
  metadata_service_impl.h / .cpp    # bucket/object API, backpressure, retry mapping
  raft_service_impl.h / .cpp

modules/raft/state_machine/
  state_machine.h / .cpp            # final state: deleted
  metadata_state_machine.h / .cpp   # V2 data model, shared_mutex, snapshot V2

modules/raft/storage/
  snapshot_storage.h / .cpp         # preserve publish semantics; no silent downgrade

tests/
  test_command.cpp                  # replace or retire KV codec coverage
  test_state_machine.cpp            # final state: retire/replace
  test_kv_service.cpp               # final state: retire/replace
  test_raft_log_replication.cpp
  test_raft_commit_apply.cpp
  test_raft_split_brain.cpp
  raft_integration_test.cpp
  snapshot_test.cpp
  persistence_test.cpp
  test_raft_snapshot_catchup.cpp
  test_raft_snapshot_restart.cpp
  test_t017_leader_switch_ordering.cpp
  metadata_*.cpp                    # upgrade to bucket/object semantics

build/test entrypoints
  CMakeLists.txt
  tests/CMakeLists.txt
  test.sh
  test.ps1
```

**Structure Decision**: 保持现有仓库布局与 Raft core module 边界，不新建大而全的替代框架。业务层切换的主轴是：
1. 先让 metadata-only 节点装配和测试装配成立；
2. 再让 metadata-only 成为默认主路径；
3. 最后删除 KV 残留文件、target、proto RPC、脚本入口和测试依赖。

## Phase 0 Research Decisions

### Decision 1: 先迁移装配，再删除默认 composite

**Decision**: 先建立 metadata-only 的测试 helper、断言 helper 和新回归入口，再把 `RaftNode` 默认构造从 `CompositeKvMetadataStateMachine` 切成 metadata-only，最终删除 composite。  
**Rationale**: 当前大量测试直接用 `RaftNode(config[, snapshot])` 默认构造，并依赖 KV 可见性；如果一开始直接删默认 composite，会导致回归入口同时失效，无法做渐进迁移。  
**Alternatives considered**:
- 立即把默认构造改成 metadata-only：被拒绝，因为现有 `test_raft_*`、`persistence_test`、`raft_integration_test` 大量依赖 `SET/DEL` 和 `DebugGetValue()`。
- 长期保留 composite：被拒绝，因为这会形成永久 KV fallback，违背 feature 目标。

### Decision 2: 把 current metadata V1 当作种子，而不是当作最终模型

**Decision**: 复用现有 `metadata_command.*`、`metadata_state_machine.*`、`metadata_service_impl.*`、`raft_metadata_client.cpp` 作为迁移起点，但在 `006` 中升级为 bucket/object/request/tombstone V2，而不是围绕 V1 打补丁。  
**Rationale**: 当前 metadata V1 已经证明“Raft 复制 metadata payload”这条链路能工作，但它只覆盖 object record，没有 bucket、abort、object index、shared read path，也没有最终的 proto/CLI 语义。  
**Alternatives considered**:
- 完全重写 metadata 模块：被拒绝，因为会丢掉现有测试积累。
- 仅在 V1 上继续加字段不改结构：被拒绝，因为 object-only map 无法自然表达 bucket/index/request 表与并发快照边界。

### Decision 3: 保留 Raft apply 顺序机制，不引入第二套共识队列

**Decision**: 继续使用 `RaftNode::ApplyCommittedEntries()` 的单调 apply 路径和 `apply_mu_` 作为全局顺序屏障，把“ordered apply queue”定义为现有 apply 序推进契约；状态机内部再用 `shared_mutex` 实现单写多读。  
**Rationale**: 现有节点已经保证日志追加、commit 推进和 apply 顺序，风险更高的是业务状态机的并发可见性和幂等更新，而不是再造一套独立 apply 线程模型。  
**Alternatives considered**:
- 新增独立 apply 队列线程：被拒绝，因为会改动更多 Raft 核心并发面。
- 让状态机自己解决乱序 apply：被拒绝，因为 apply 顺序必须由节点和 log index 驱动。

### Decision 4: 保留 SnapshotStorage 的发布语义，升级 metadata snapshot 数据文件为 V2

**Decision**: 保留 `SnapshotStorage` 的 staging publish、目录同步和 checksum 验证语义；metadata state machine 只升级自己的 snapshot 数据文件格式为 V2，并在数据头中显式记录 `last_applied_index/term`、bucket/object/request/table counts。  
**Rationale**: crash-safe 目录发布已经在 `snapshot_storage.cpp` 中实现，问题在于 metadata data file 还不包含足够的自描述边界，无法校验“snapshot.meta 与 metadata.snapshot 是否同一 apply 点”。  
**Alternatives considered**:
- 在状态机内部自己做目录发布：被拒绝，因为会复制已有 durability 代码。
- 继续只依赖外层 meta、不升级 data file：被拒绝，因为无法对 state machine 内容和 meta 边界做一致性校验。

### Decision 5: 删除 KvService，但保留非 KV 的管理面诊断能力

**Decision**: `KvService` 及其 `Put/Get/Delete` 业务接口被彻底删除；`Status/Health/Metrics` 迁移到新的非-KV 管理面契约，而不是继续寄存在 `KvService` 名下。  
**Rationale**: observability 是 constitution 明确要求；如果连同 `KvService` 一起删除状态/健康/指标而不补位，会削弱恢复与切主诊断能力。  
**Alternatives considered**:
- 保留 `KvService` 仅承载状态接口：被拒绝，因为仍会保留 KV 名义主路径。
- 把状态接口塞进 `MetadataService`：可行但不优先，因为业务面与管理面会混在一起。

### Decision 6: 允许业务层 public/proto/snapshot surface 发生显式破坏性迁移，但不承诺 KV 向 metadata 的运行时兼容

**Decision**: `006` 不保留 KV compatibility，也不承诺从旧 KV data dir、旧 metadata V1 snapshot、旧 KV proto client 做无缝升级；实现会用显式版本错误、干净数据目录和全量回归迁移来收敛边界。  
**Rationale**: 该 feature 的目标就是彻底删除 KV 模型；继续维持运行时双兼容会把复杂度锁死在过渡层。  
**Alternatives considered**:
- 提供 KV->metadata 自动在线迁移：被拒绝，因为超出 feature 范围，且会引入持久化格式迁移和历史语义映射。
- 对旧 snapshot 静默 best-effort 读取：被拒绝，因为违反 durability / explicit-error 原则。

### Decision 7: 先迁移高价值 Raft 回归，再删 KV 测试和脚本 fallback

**Decision**: 先把 `test_raft_log_replication`、`test_raft_commit_apply`、`snapshot_test`、`persistence_test`、`test_raft_snapshot_restart`、`test_t017_leader_switch_ordering` 等核心回归从 KV 断言迁移到 metadata 断言，再删除 `test_state_machine`、`test_kv_service` 与 `test.ps1` 中的 KV fallback 子集。  
**Rationale**: 当前高价值回归测试对 Raft 核心依赖深，不能直接删除；它们必须先换成 metadata 主路径，最后才能安全移除 KV 专用入口。  
**Alternatives considered**:
- 先删所有 KV 测试再补 metadata：被拒绝，因为会丢失稳定回归面。
- 同时改所有测试和节点装配：被拒绝，因为变更面过大、难定位故障。

## Phase 1 Design

### 1. Node Assembly Refactor

目标是把“metadata-only”做成 `RaftNode` 的唯一业务装配模式。

- 删除 `CompositeKvMetadataStateMachine`
- 默认 `RaftNode(config)` / `RaftNode(config, snapshotConfig)` 改为装配 `MetadataStateMachine`，不再暗含 KV
- `InitServer()` 最终只注册 `RaftService`、`MetadataService`、非-KV 管理面服务
- `DebugGetValue()`、`Describe()` 中的 KV 视图片段从主路径删除，改为 metadata 诊断摘要
- 需要保留的调试能力通过 metadata 查询 helper 或 node status snapshot 暴露，而不是通过 KV map 侧信道暴露

### 2. Metadata Command and Data Model V2

当前 V1 只支持 object-record 级 `create/commit/delete`。V2 需要扩展为：

- `CreateBucket`
- `DeleteBucket`
- `CreateObject`
- `CommitObject`
- `AbortObject`
- `DeleteObject`
- `HeadObject`
- `ListObjects`

设计要求：

- Raft 日志只复制 metadata command，不复制真实对象数据
- `MetadataCommand` V2 直接表达 bucket/object 生命周期
- `request_id` 与对象状态变更在同一个 apply 临界区内更新
- 删除或中止后必须保留足够的 request/lifecycle facts，拒绝 stale retry
- `ObjectState` 对外只保留 `PENDING / COMMITTED / DELETED`

### 3. MetadataStateMachine V2 Concurrency Model

- 单个 committed log index 只能被 apply 一次
- apply 路径保持单写；状态机内部使用 `std::shared_mutex`
- `Apply()` 持有写锁，按 `RaftNode` 提供的 log index 顺序串行执行
- `HeadObject` / `ListObjects` 持有读锁
- `SaveSnapshot()` 在 node 持有 `apply_mu_` 的前提下获取读锁，复制一致性视图后释放锁，再执行文件 I/O
- 状态机内部显式保存 `last_applied_index` / `last_applied_term`，并作为 snapshot V2 header 的一部分

### 4. Snapshot / Restart / Catch-up Design

- 保留 `SnapshotStorage` 目录发布、校验和、平台 durability contract
- 升级 metadata snapshot data file 到 V2，头部记录：
  - snapshot version
  - `last_applied_index`
  - `last_applied_term`
  - bucket/object/object_index/chunk_ref/request/tombstone 计数
- `SnapshotStorage` 外层 meta 与 metadata snapshot 内层 header 的 apply 边界必须一致
- `LoadSnapshot()` 成功后，`RaftNode` 继续 replay `index > last_applied_index` 的日志
- follower catch-up 与 leader restart 继续复用现有 log/snapshot orchestration，不新建另一套恢复机制

### 5. Public Contract Design

- `RaftService` 保持不变
- `KvService` 从主 proto 移除
- `MetadataService` 改成 bucket/object 语义 RPC
- 当前 `PutRequest` / `GetRequest` / `DeleteRequest` 等 KV message 删除
- `Status/Health/Metrics` 迁移到非-KV 管理面契约
- metadata 状态码增加显式 overload/backpressure 表达，避免 overloaded 请求被误报成内部错误

### 6. Client and Script Migration

- 删除 `raft_kv_client`
- 扩展已有 `raft_metadata_client`：
  - `create-bucket`
  - `delete-bucket`
  - `create-object`
  - `commit-object`
  - `abort-object`
  - `delete-object`
  - `head-object`
  - `list-objects`
- 保留现有 retry/redirect 行为，但全部改为 bucket/object 语义
- `test.sh` 删除 `kv-service` 分组并替换为 metadata/service/regression 组合
- `test.ps1` 的 conservative fallback 从 `KvStateMachineTest` 子集迁移到 metadata-only 子集

### 7. Test Migration Strategy

先迁移核心，再删 KV：

1. 建立 metadata test helper  
   包括 bucket/object request builder、metadata assert helper、leader query helper、cluster fixture helper
2. 迁移直接业务测试  
   `test_command.cpp`、`test_state_machine.cpp`、`test_kv_service.cpp`
3. 迁移高价值 Raft 回归  
   `test_raft_log_replication.cpp`、`test_raft_commit_apply.cpp`、`raft_integration_test.cpp`、`snapshot_test.cpp`、`persistence_test.cpp`、`test_raft_snapshot_catchup.cpp`、`test_raft_snapshot_restart.cpp`、`test_t017_leader_switch_ordering.cpp`、`test_raft_split_brain.cpp`
4. 迁移 metadata 现有测试到 V2 bucket/object 语义  
   `metadata_command_test.cpp`、`metadata_state_machine_test.cpp`、`metadata_snapshot_test.cpp`、`metadata_failover_test.cpp`、`metadata_client_scenario_test.cpp`
5. 删除 KV-only 测试 target 与 fallback

### 8. Observability and Backpressure

- 当前 `RpcKind` 只覆盖 KV RPC；需要补齐 metadata/business/admin RPC metrics
- metadata 写入口增加 bounded admission，超限返回显式 overload
- timeout、not-leader、idempotent replay、idempotency conflict、state conflict 需要有稳定的 response code 和 message
- node/status 描述输出改为 metadata-oriented，而不是 `kv={...}`

## Phase 2 Task Breakdown Guidance

后续 `/speckit-tasks` 建议按以下顺序拆分，保持每一阶段都可单独编译与验证：

1. **装配勘察与 helper 搭建**  
   新增 metadata cluster/test helper，明确所有 `RaftNode` 默认实例化点和 KV 断言点。

2. **Metadata 数据模型 V2**  
   扩展 `metadata_command.*`、`metadata_result.*`、`metadata_state_machine.*` 到 bucket/object/request/tombstone/index 模型，补 `AbortObject` 与 bucket 语义。

3. **状态机并发与 snapshot V2**  
   引入 `shared_mutex`、内部 `last_applied_index/term`、snapshot V2 header、自校验与 V1 显式拒绝路径。

4. **节点 metadata-only 装配**  
   改 `RaftNode` 默认构造、删除 composite、更新 `Describe()` / debug helper / metrics kind，保证 metadata-only 仍能复用现有 replication/commit/apply/recovery。

5. **proto / service / admin 契约迁移**  
   移除 `KvService`，引入新的 metadata object/bucket RPC 与非-KV 管理面 RPC，更新 service impl 映射与 backpressure/timeout/redirect 行为。

6. **客户端与构建系统迁移**  
   升级 `raft_metadata_client`、删除 `raft_kv_client` target、清理 `CMakeLists.txt` 和 `tests/CMakeLists.txt` 的 KV target。

7. **高价值 Raft 回归迁移**  
   逐个把 `SET/DEL` 测试改成 metadata command / metadata read assertions，替换 `DebugGetValue()` 依赖。

8. **脚本与平台入口迁移**  
   更新 `test.sh`、`test.ps1`、`ctest` labels/presets，使 Linux/Windows 主入口不再包含 KV fallback。

9. **最终 KV 删除与文档清理**  
   删除 `state_machine.*` KV 实现、`kv_service_impl.*`、KV proto/message、KV tests 和可读文档中的 KV 主路径描述；README 另行按仓库限制处理。

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| 删除 `KvService` 并迁移 `Status/Health/Metrics` 到非-KV 管理面 | feature 明确禁止保留 KV service，但 constitution 又要求保留 observability | 继续保留 `KvService` 仅做管理面会形成 KV compatibility surface |
| 升级 metadata snapshot data file 到 V2 | bucket/object/request/tombstone/index + applied boundary 无法塞进现有 object-only `MDS1` | 继续复用 V1 格式无法表达新模型，也无法校验内外层 snapshot 边界一致性 |

## Post-Design Constitution Check

- **Preserve The Verified Core**: 通过。计划不重写选举、复制、SegmentLogStorage、SnapshotStorage，只替换业务层与验证入口。
- **Durability Contract Before Convenience**: 通过。明确复用 `SnapshotStorage` durability contract，并要求 metadata snapshot V2 显式版本化和边界自校验。
- **Recovery And Consistency First**: 通过。计划覆盖 ordered apply、restart replay、snapshot consistency、follower catch-up、leader switch 和 duplicate request handling。
- **Cross-Platform By Default, Linux By Primary Validation**: 通过。Linux 继续主验证；Windows fallback/test preset 也要迁移到 metadata-only，而不是停留在 KV subset。
- **Observability And Minimal Surface Change**: 通过但有显式 surface 迁移。`KvService` 删除会带来 proto/public target 变更，因此用非-KV 管理面和增量测试迁移收敛风险，而不是静默保留旧入口。
