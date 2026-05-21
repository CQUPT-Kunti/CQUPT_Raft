# T001 KV 依赖清单梳理

## 1. T001 结论

- 当前仓库不存在名为 `KVCommand` 的独立类型；KV 命令面实际落在 `modules/raft/common/command.h/.cpp` 的 `Command`、`CommandType::kSet/kDelete`、`SET|key|value` / `DEL|key|` 编解码上。
- KV 主路径仍然是活跃依赖，不只是遗留文件：`RaftNode` 默认装配 `CompositeKvMetadataStateMachine`，gRPC 同时注册 `KvService` 与 `MetadataService`，Linux/Windows 默认测试入口仍显式包含 `KvStateMachineTest` / `RaftKvServiceTest`。
- KV 依赖可分为五层：源码依赖、测试依赖、CMake 依赖、proto 依赖、AGENTS/feature 文档依赖；其中耦合最深的是 `RaftNode` 默认状态机装配、`KvService` 状态/健康/指标接口、以及大量 Raft 回归测试对 `DebugGetValue()` 与 `CommandType::kSet/kDelete` 的依赖。
- 必须删除的对象已经明确：KV 命令面、`KvStateMachine`、`KvService`、`raft_kv_client`、KV target、KV RPC、KV-only 测试。
- 必须迁移的对象已经明确：所有本质上在验证 Raft election/replication/commit/apply/snapshot/restart/catch-up/failover/concurrency 的测试，只是目前借用了 KV 断言。

## 2. 已读取的根 AGENTS.md 和子模块 AGENTS.md

- 根 `AGENTS.md`
- `modules/raft/common/AGENTS.md`
- `modules/raft/service/AGENTS.md`
- `modules/raft/node/AGENTS.md`
- `modules/raft/state_machine/AGENTS.md`
- `modules/raft/storage/AGENTS.md`
- `modules/raft/replication/AGENTS.md`
- `modules/raft/runtime/AGENTS.md`
- `proto/AGENTS.md`
- `apps/AGENTS.md`
- 同时已读取：
  - `specs/006-remove-kv-metadata-state-machine/spec.md`
  - `specs/006-remove-kv-metadata-state-machine/plan.md`
  - `specs/006-remove-kv-metadata-state-machine/tasks.md`
  - `specs/005-strong-consistency-metadata-layer/plan.md`

## 3. KV 相关文件清单

### 源码层

- `modules/raft/common/command.h`
- `modules/raft/common/command.cpp`
- `modules/raft/service/kv_service_impl.h`
- `modules/raft/service/kv_service_impl.cpp`
- `modules/raft/state_machine/state_machine.h`
- `modules/raft/state_machine/state_machine.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `apps/raft_kv_client.cpp`

### 元数据并存层

- `modules/raft/common/metadata_command.h`
- `modules/raft/common/metadata_command.cpp`
- `modules/raft/common/metadata_result.h`
- `modules/raft/service/metadata_service_impl.h`
- `modules/raft/service/metadata_service_impl.cpp`
- `modules/raft/state_machine/metadata_state_machine.h`
- `modules/raft/state_machine/metadata_state_machine.cpp`
- `apps/raft_metadata_client.cpp`

### proto / 构建 / 脚本

- `proto/raft.proto`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `test.sh`
- `test.ps1`

### 测试层

- KV-only：
  - `tests/test_state_machine.cpp`
  - `tests/test_kv_service.cpp`
- metadata 现有测试：
  - `tests/metadata_command_test.cpp`
  - `tests/metadata_state_machine_test.cpp`
  - `tests/metadata_snapshot_test.cpp`
  - `tests/metadata_failover_test.cpp`
  - `tests/metadata_client_scenario_test.cpp`
  - `tests/metadata_manifest_test.cpp`
- 使用 KV 断言的 Raft 回归测试：
  - `tests/test_command.cpp`
  - `tests/test_raft_election.cpp`
  - `tests/test_raft_log_replication.cpp`
  - `tests/test_raft_commit_apply.cpp`
  - `tests/raft_integration_test.cpp`
  - `tests/snapshot_test.cpp`
  - `tests/persistence_test.cpp`
  - `tests/persistence_more_test.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/test_raft_split_brain.cpp`
  - `tests/test_raft_replicator_behavior.cpp`
  - `tests/test_raft_segment_storage.cpp`
  - `tests/test_t017_leader_switch_ordering.cpp`

## 4. KV 符号引用清单

### 命令面

- `CommandType::kSet` / `CommandType::kDelete` / `CommandType::kMetadata`：`modules/raft/common/command.h`
- `SET|key|value` / `DEL|key|` / `META|payload_size|payload`：`modules/raft/common/command.cpp`
- 结论：`KVCommand` 的逻辑实体不是独立类，而是 `Command` 的 KV 分支。

### 状态机面

- `KvStateMachine`：`modules/raft/state_machine/state_machine.h/.cpp`
- `CompositeKvMetadataStateMachine`：`modules/raft/node/raft_node.h/.cpp`
- `DebugGetValue()`：`modules/raft/node/raft_node.h/.cpp`，被大量测试直接使用
- `Describe()` 中 `kv=` 视图：`modules/raft/node/raft_node.cpp`

### 服务与客户端

- `KvServiceImpl`：`modules/raft/service/kv_service_impl.h/.cpp`
- `raft::KvService::CallbackService`：`modules/raft/service/kv_service_impl.h`
- `raft_kv_client` / `raft::KvService::Stub`：`apps/raft_kv_client.cpp`
- `Status/Health/Metrics` 当前仍挂在 `KvService`

### 节点装配

- 默认 `RaftNode` 构造创建 `CompositeKvMetadataStateMachine`：`modules/raft/node/raft_node.cpp`
- `InitServer()` 同时注册 `RaftService`、`KvService`、`MetadataService`：`modules/raft/node/raft_node.cpp`
- `GetMetadataStateMachine()` 兼容直接 metadata 状态机和 composite 状态机：`modules/raft/node/raft_node.cpp`

## 5. CMake / proto / client / service / test 依赖

### CMake 依赖

- `raft_core` 仍编译：
  - `modules/raft/service/kv_service_impl.cpp`
  - `modules/raft/state_machine/state_machine.cpp`
- 根 `CMakeLists.txt` 仍定义：
  - `add_executable(raft_kv_client apps/raft_kv_client.cpp)`
- `tests/CMakeLists.txt` 仍注册：
  - `test_state_machine`
  - `test_kv_service`
  - 多个 KV 断言驱动的 Raft 回归测试

### proto 依赖

- `proto/raft.proto` 同时定义：
  - `service KvService`
  - `service MetadataService`
- `KvService` RPC：
  - `Put`
  - `Delete`
  - `Get`
  - `Status`
  - `Health`
  - `Metrics`
- `MetadataService` 仍是 record-centric V1：
  - `CreateMetadataRecord`
  - `CommitMetadataRecord`
  - `DeleteMetadataRecord`
  - `HeadMetadataRecord`
  - `ListMetadataRecords`

### client 依赖

- `apps/raft_kv_client.cpp` 是完整 KV CLI
- `tests/metadata_client_scenario_test.cpp` 依赖 `raft_metadata_client`
- 当前 client 层处于 KV 与 metadata 双入口并存状态

### service 依赖

- `modules/raft/service/kv_service_impl.*` 直接依赖 `RaftNode`
- `modules/raft/service/metadata_service_impl.*` 也直接依赖 `RaftNode`
- 当前 `RaftNode` 服务注册是双栈并存，不是 metadata-only

### test / script 依赖

- `test.sh`：
  - `unit` 分组仍包含 `KvStateMachineTest`
  - 单独保留 `kv-service` 分组，对应 `RaftKvServiceTest`
- `test.ps1`：
  - Windows fallback subset 仍明确写死 `KvStateMachineTest`
- 结论：默认测试入口仍把 KV 作为一等验证路径

## 6. 测试依赖分类

### A. KV-only，可删除

- `tests/test_state_machine.cpp`
  - 纯 `KvStateMachine` 行为测试
- `tests/test_kv_service.cpp`
  - 纯 `KvService`/`Put/Get/Delete/Status/Health` 路径测试
- `test.sh` 中 `kv-service` 分组
- `test.ps1` 中 `KvStateMachineTest` fallback 宣传与默认子集

### B. 本质是 Raft 回归，必须迁移到 MetadataStateMachine 或 metadata 查询断言

- log replication / commit / apply：
  - `tests/test_raft_log_replication.cpp`
  - `tests/test_raft_commit_apply.cpp`
  - `tests/raft_integration_test.cpp`
  - `tests/test_t017_leader_switch_ordering.cpp`
- leader election / split brain / failover：
  - `tests/test_raft_election.cpp`
  - `tests/test_raft_split_brain.cpp`
  - `tests/test_raft_replicator_behavior.cpp`
- snapshot / restart / recovery：
  - `tests/snapshot_test.cpp`
  - `tests/persistence_test.cpp`
  - `tests/persistence_more_test.cpp`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/test_raft_segment_storage.cpp`
- follower catch-up：
  - `tests/test_raft_snapshot_catchup.cpp`
- 说明：
  - 这些测试当前依赖 `CommandType::kSet/kDelete`、`DebugGetValue()` 或 `KvStateMachine snapshot`；
  - 但它们验证的核心能力属于 Raft baseline，不应随 KV 一起删除。

### C. 需保留并升级的 metadata 测试

- `tests/metadata_command_test.cpp`
- `tests/metadata_state_machine_test.cpp`
- `tests/metadata_snapshot_test.cpp`
- `tests/metadata_failover_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `tests/metadata_manifest_test.cpp`
- 说明：
  - 当前 metadata 测试覆盖的是 record-centric V1，不是目标 bucket/object V2；
  - 后续需要扩展，而不是直接删除。

## 7. 必须删除项

- KV 命令面：
  - `modules/raft/common/command.h/.cpp` 中的 `kSet` / `kDelete` / `SET|...` / `DEL|...`
- `KvStateMachine`：
  - `modules/raft/state_machine/state_machine.h/.cpp`
- `KvService`：
  - `modules/raft/service/kv_service_impl.h/.cpp`
- `raft_kv_client`：
  - `apps/raft_kv_client.cpp`
  - 根 `CMakeLists.txt` 中 `raft_kv_client` target
- KV proto/RPC：
  - `proto/raft.proto` 中 `service KvService`
  - `Put/Get/Delete/Status/Health/Metrics` 相关 message/RPC
- KV-only tests：
  - `tests/test_state_machine.cpp`
  - `tests/test_kv_service.cpp`
  - `test.sh` 的 `kv-service` 分组
  - `test.ps1` 的 `KvStateMachineTest` fallback

## 8. 必须迁移项

- 状态机装配：
  - `CompositeKvMetadataStateMachine`
  - `RaftNode` 默认构造中的 KV+metadata composite 注入
- 调试/查询断言面：
  - `DebugGetValue()`
  - `Describe()` 中 `kv=` 片段
- 所有借用 KV 断言的高价值 Raft 回归测试：
  - log replication
  - commit/apply
  - snapshot/restart recovery
  - follower catch-up
  - split brain / leader switch
  - 并发或恢复边界测试
- proto / service / client：
  - `MetadataService` 从 record-centric V1 升级为 bucket/object V2
  - `raft_metadata_client` 升级为唯一业务 CLI
- 管理面能力：
  - `Status/Health/Metrics` 不能跟着 `KvService` 一起丢失，必须迁移到 non-KV 管理面

## 9. 待确认项

- `IStateMachine` 抽象边界是否继续复用现有接口，还是仅替换实现类型
- `RaftNode` 默认装配翻转后，`DebugGetValue()` 是否直接删除，还是保留 metadata-oriented 调试替代接口
- `proto/raft.proto` 中 `Status/Health/Metrics` 是迁到独立 admin service，还是并入新的 non-KV 管理面
- `tests/test_command.cpp` 的去向：
  - 删除 KV codec 断言
  - 还是改为 metadata payload / wrapper 行为断言
- `tests/test_raft_split_brain.cpp` 中直接实例化 `KvStateMachine snapshot_state` 的局部测试逻辑，需要重设计而不是简单替换断言
- `metadata_state_machine` 当前 V1 snapshot 只保存 `records_ / tombstones_ / replay_table_`，与 `006` 目标中的 `bucket_table / object_index / request_table` 不一致，后续恢复测试需要重写断言边界
- `src/`、`include/`、`cmake/` 目录在当前仓库根下不存在，本次调查实际落点为 `modules/`、`tests/`、`proto/`、`apps/`、根 `CMakeLists.txt`、脚本与 AGENTS/spec 文档

## 10. 后续需要同步更新的 AGENTS.md 清单

- 根 `AGENTS.md`
  - 当前仍把项目定位为 “Raft KV 内核项目”
  - 当前模块索引仍写 `gRPC Raft/KV 服务适配层`、`KV 状态机与状态机快照格式`
  - 当前 CMake target 示例仍含 `raft_kv_client`
- `modules/raft/common/AGENTS.md`
  - 当前职责仍写 “保持 `SET|key|value`、`DEL|key|` 命令格式”
- `modules/raft/service/AGENTS.md`
  - 当前 Scope 仍写 `gRPC Raft/KV 服务适配层`
  - Files 中仍列出 `kv_service_impl.*`
- `modules/raft/state_machine/AGENTS.md`
  - 当前整体描述仍是 KV 状态机与快照格式
  - Files / Relevant Tests / Context Hints 都仍指向 `state_machine.*`
- `proto/AGENTS.md`
  - 当前 Responsibilities 仍写 “定义 `KvService`”
  - Risk Areas 仍写 “Raft RPC 与 KV RPC 的兼容性”
- `apps/AGENTS.md`
  - Files 中仍列出 `raft_kv_client.cpp`
  - Context Hints 仍指向 `raft_kv_client.cpp`
- `modules/raft/node/AGENTS.md`
  - 后续需要补充 metadata-only 状态机装配、管理面服务与迁移后的测试关注点
- `modules/raft/storage/AGENTS.md`
  - 需要把 “不负责 KV apply 语义” 改成更一般的 metadata-only 业务边界表述

## 11. 风险点

- 最大耦合点不在单个 KV 文件，而在 `RaftNode` 默认 composite 装配和测试默认入口；如果先删 KV 文件，不先迁移装配和断言，主回归会整体失效。
- `Status/Health/Metrics` 当前绑在 `KvService`，删除 KV RPC 时容易误删 observability。
- `test.sh` 与 `test.ps1` 都把 KV 作为默认或保守入口的一部分，说明 CI/人工验证路径存在隐藏依赖。
- 不是所有 “看起来像 KV 测试” 都能删除；一批测试实际上在验证持久化、快照边界、follower catch-up、restart recovery。
- `test_raft_split_brain.cpp` 既有 KV 断言又有局部 `KvStateMachine` 直接使用，是迁移复杂度最高的测试之一。
- metadata V1 已存在，但仍是 object-record 模型；直接把 KV 测试替换成当前 V1 metadata 断言，无法覆盖 `006` 目标里的 bucket/object/request/tombstone 语义。

## 12. 验收结果

- 已生成本报告：`specs/006-remove-kv-metadata-state-machine/task-reports/t001-kv-dependency-inventory.md`
- 已列出 `KVCommand` 等价实现、`KvStateMachine`、`KvService`、`raft_kv_client` 的主要引用点
- 已区分源码、测试、CMake、proto、AGENTS/文档依赖
- 已给出必须删除 / 必须迁移 / 待确认分类
- 已列出后续需要同步更新的 `AGENTS.md`
- 本次未修改源码、未修改测试、未修改 CMake、未修改 proto
- 本次未修改任何 `AGENTS.md`、`spec.md`、`plan.md`、`tasks.md`
- 本次未执行 T002 或任何后续任务
- Linux 结果：仅完成静态依赖调查，未运行构建/测试
- Windows 结果：仅完成静态依赖调查，未运行构建/测试
- CTest 结果：未执行
