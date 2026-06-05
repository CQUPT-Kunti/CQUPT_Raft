# T034 Placement Manager

## 修改文件

- `modules/store/placement/placement_manager.h`
- `modules/store/placement/placement_manager.cpp`
- `modules/store/placement/module-notes.md`
- `modules/store/placement/AGENTS.md`
- `tests/store_placement_manager_test.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `PlacementManager`，承接一次静态候选节点 placement 调用
- `PlacementManager` 复用 `ReplicaPolicySelector::SelectReplicas()`，不复制筛选和排序逻辑
- 在 manager 层补充 `decision.reasons` 摘要，输出候选数、策略参数和显式 excluded 节点数量
- 新增 `store_placement_manager` 单测，覆盖副本数选择、显式排除、排除原因可观察和节点不足失败路径

## PlacementManager 输入、输出和 decision reasons 语义

- 输入：
  - `PlacementRequest`
    - `chunk_id` 或 `object_id/version/chunk_index`
    - `chunk_size_bytes`
    - `ReplicaPolicy`
    - `excluded_nodes`
    - `decision_epoch`
  - `std::span<const StorageNodePlacementCandidate>`
    - 静态候选节点列表
- 输出：
  - `PlacementDecisionResult`
  - `decision.replica_nodes`
    - 最终选中的副本节点
  - `decision.required_replica_count`
  - `decision.minimum_successful_writes`
  - `decision.excluded_nodes`
    - 显式排除、健康不可写、磁盘压力、容量不足、过载等原因
  - `decision.reasons`
    - manager 摘要
    - selector 的排序、zone spread 或节点不足说明
- 当前 manager 摘要包括：
  - 评估了多少个静态候选节点
  - `replica_count` 和 `minimum_successful_writes`
  - 调用方显式排除了多少个节点

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_placement|placement_manager|placement_policy" --output-on-failure 2>&1 | tee tmp/007/t034-placement-test.log`
  - PASS
  - 日志路径：`tmp/007/t034-placement-test.log`

## Windows 验证判断

- 本任务只新增平台无关的 placement manager 和单测
- 未新增 `T034-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T034

- 是

## 是否可以进入 T035

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- 当前 `PlacementManager` 仍是静态候选协调层，没有 heartbeat / registry / upload coordinator
- T027 的 upload coordinator / orphan chunk 风险仍未解决，本任务不误删

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/placement/module-notes.md`
- 更新了 `modules/store/placement/AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T034 标记完成，并把测试路径纠偏为 `tests/store_placement_manager_test.cpp`

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
