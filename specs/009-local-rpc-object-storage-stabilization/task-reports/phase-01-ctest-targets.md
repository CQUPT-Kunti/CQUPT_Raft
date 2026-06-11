# T003 Phase 1 CTest / Target / Label Survey

## Scope

本任务只确认 `tests/CMakeLists.txt` 中当前真实存在的 build target、CTest 入口、custom target 和 label，并把结果回填到 `validation-matrix.md`。未修改生产代码、测试代码和 `tests/CMakeLists.txt`。

## Read Set

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/quickstart.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/local-rpc-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/research.md`
- `specs/009-local-rpc-object-storage-stabilization/data-model.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/phase-01-survey.md`
- `tests/AGENTS.md`
- `tests/CMakeLists.txt`

## Text Checks

已执行文本核对：

```bash
test -f tests/CMakeLists.txt
rg -n "test_view_node_discovery|test_node_identity|cluster_config_test|storage_heartbeat_registry|integrated_object_storage_e2e|integrated_object_storage_quorum|integrated_object_storage_recovery|integrated_object_storage_concurrency|metadata_failover_test|metadata_client_scenario_test|test_raft_election|test_raft_log_replication|test_raft_snapshot_catchup|test_raft_snapshot_restart" tests/CMakeLists.txt
rg -n "LABELS|set_tests_properties|add_test\\(|add_custom_target\\(" tests/CMakeLists.txt
```

关键定位：

- label 定义：`tests/CMakeLists.txt:53-99`
- `add_store_ctest()`：`tests/CMakeLists.txt:102-108`
- `storage_heartbeat_registry`：`tests/CMakeLists.txt:762-779`
- Raft election / replication / snapshot：`tests/CMakeLists.txt:832-865`
- integrated object storage e2e / quorum / recovery / concurrency：`tests/CMakeLists.txt:895-970`
- ViewNode / identity / cluster config：`tests/CMakeLists.txt:973-987`

## CTest Listing

已执行仅列测试，不运行：

```bash
ctest --preset debug-ninja-low-parallel -N
ctest --preset debug-tests -N
```

结果：

- `ctest --preset debug-ninja-low-parallel -N`
  - 跳过原因：仓库里不存在该 test preset。
- `ctest --preset debug-tests -N`
  - 成功列出当前 `build/linux` 中的 276 个 CTest 入口。

## Confirmed Findings

### 1. `storage_heartbeat_registry` 是精确 CTest test name

- build target：`test_storage_heartbeat_registry`
- CTest test name：`storage_heartbeat_registry`
- labels：`storage-node;platform-neutral`

原因：该入口通过 `add_store_ctest()` 包装 `add_test(NAME storage_heartbeat_registry ...)` 注册。

### 2. 多数 009 文档里写的 `test_*` 名称当前是 build target，不是最终 CTest case name

已确认：

- `test_view_node_discovery` -> `ViewNodeDiscoveryTest.*`
- `test_node_identity` -> `NodeIdentityTest.*`
- `cluster_config_test` -> `cluster_config_generation_test.*` / `cluster_config_validation_test.*` / `cluster_config_endpoint_allocation_test.*` / `cluster_config_resolution_test.*` / `cluster_config_quorum_helper_test.*`
- `test_integrated_object_storage_e2e` -> `IntegratedObjectStorageE2ETest.*`
- `test_integrated_object_storage_quorum` -> `IntegratedObjectStorageQuorumTest.*`
- `test_raft_election` -> `RaftElectionTest.*`
- `test_raft_log_replication` -> `RaftLogReplicationTest.*`
- `test_raft_snapshot_catchup` -> `RaftSnapshotCatchupTest.*`
- `test_raft_snapshot_restart` -> `RaftSnapshotRestartTest.*` 与 `RaftSnapshotRecoveryTest.*`
- `test_metadata_failover` -> `MetadataFailoverTest.*`
- `test_metadata_client_scenario` -> `MetadataClientScenarioTest.*`

影响：

- 后续 targeted build 仍可直接使用这些 build target。
- 后续 targeted CTest 不应把这些 build target 当成精确 test name；应改用 label 或 case regex。

### 3. 已确认存在的 custom target

- `integrated_object_storage_e2e`
- `integrated_object_storage_quorum`
- `integrated_object_storage_recovery`
- `integrated_object_storage_concurrency`

### 4. 已确认重点 label

- `integrated-object-storage`
- `integrated-object-storage-e2e`
- `integrated-object-storage-quorum`
- `integrated-object-storage-recovery`
- `integrated-object-storage-concurrency`
- `view-node`
- `node-identity`
- `storage-node`
- `platform-neutral`
- `linux-primary-diagnosis`

本任务要求重点检查的 label 没有发现“未确认到”项。

## Differences Against Existing 009 Docs

- 009 若干文档把 `test_view_node_discovery`、`test_node_identity`、`cluster_config_test`、`test_integrated_object_storage_e2e`、`test_integrated_object_storage_quorum` 写成 “CTest test name”；按当前 `tests/CMakeLists.txt` 和 `ctest --preset debug-tests -N` 结果，它们更准确地说是 build target。
- `storage_heartbeat_registry` 是一个例外：它既有 build target `test_storage_heartbeat_registry`，也有精确的 CTest test name `storage_heartbeat_registry`。
- 用户提示里的 `debug-ninja-low-parallel` 适合作为 build preset 命名参考，但当前仓库里未配置同名 test preset。

## Files Updated

- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`

## Build / Test Execution

- 文本检查：已执行
- CTest listing：已执行
- configure/build：未执行
- test run：未执行

原因：T003 只要求确认入口，不要求构建或运行测试。
