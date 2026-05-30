# T033 Placement Policy

## 修改文件

- `modules/store/placement/replica_policy.h`
- `modules/store/placement/replica_policy.cpp`
- `modules/store/placement/module-notes.md`
- `modules/store/placement/AGENTS.md`
- `modules/store/AGENTS.md`
- `tests/store_placement_policy_test.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `modules/store/placement` 子模块，落地最小 `ReplicaPolicy` / `PlacementRequest` / `PlacementDecision` / `ReplicaPolicySelector`
- 新增节点候选模型，覆盖节点健康、磁盘压力、容量、负载、写入过载和 zone/rack 占位字段
- 实现纯策略 `SelectReplicas(...)`，用于后续 upload coordinator 选择候选 StorageNode 副本节点
- 新增单测，固定健康筛选、容量/负载排序、节点不足、重复节点去重和最小 zone spread 行为
- 更新 `tasks.md`，将 T033 标记完成并把测试路径纠偏为 `tests/store_placement_policy_test.cpp`

## Placement / ReplicaPolicy 字段和选择语义

- 候选节点字段：
  - `node_id`
  - `endpoint`
  - `health`
  - `disk_pressure`
  - `total_capacity_bytes`
  - `used_capacity_bytes`
  - `available_capacity_bytes`
  - `load.active_reads`
  - `load.active_writes`
  - `load.queued_ops`
  - `write_admission_overloaded`
  - `zone`
  - `rack`
- 副本策略字段：
  - `replica_count`
  - `minimum_successful_writes`
  - `avoid_same_node`
  - `prefer_distinct_zones`
  - `reserve_capacity_bytes`
- 当前选择语义：
  - 只选择 `Healthy` 节点
  - 不选择 `Degraded` / `ReadOnly` / `Unavailable` / `Draining`
  - 不选择 `disk_pressure = High / Full`
  - 不选择 `write_admission_overloaded = true`
  - 不选择容量不足以容纳 `chunk_size_bytes + reserve_capacity_bytes` 的节点
  - 不重复选择同一 `node_id`
  - 默认排序为：可用容量更大优先、总 inflight 负载更低优先、active writes 更低优先、active reads 更低优先、最后按 `node_id` 字典序稳定排序
  - `prefer_distinct_zones = true` 时，会先尽量跨 zone 选点，再按常规排序补齐
- 节点不足时返回 `kNodeUnavailable`
- 空节点列表、`replica_count = 0`、`minimum_successful_writes = 0` 或 `minimum_successful_writes > replica_count` 返回 `kInvalidArgument`

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_placement|placement_policy" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_client|storage_node_service|write_chunk_contract" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "storage_upload|local_disk_chunk_store|store_concurrency_stress" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS

## Windows 验证判断

- 本任务只新增平台无关的 placement 策略和单测，没有新增 Windows 专属行为
- 因此本次不新增 `T033-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T033

- 是

## 是否可以进入 T034

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- 当前 placement 仍是纯本地策略层，没有真实 heartbeat / registry / failure cache / upload coordinator
- `prefer_distinct_zones` 已有最小支持，但 `rack` 仍是扩展字段占位，还没有单独策略

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/placement/module-notes.md`
- 更新了 `modules/store/placement/AGENTS.md`
- 更新了 `modules/store/AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T033 标记完成，并把测试路径纠偏为 `tests/store_placement_policy_test.cpp`

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
