# T060 Health-Aware Placement Test

## 修改文件

- `tests/store_placement_policy_test.cpp`
- `tests/store_placement_manager_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t060-health-aware-placement-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `store_placement_policy` 中补充 health-aware placement 纯策略测试，固定 writable health、disk pressure、reserve capacity、稳定排序和低负载优先边界。
- 在 `store_placement_manager` 中补充 test-only registry facts -> placement candidate 适配测试，用 `last_seen + timeout` 推导 stale facts，并把 stale 节点降级为不可写事实后交给现有 manager/selector 决策。
- 将 `tasks.md` 的 T060 路径和 Heartbeat/Placement 测试矩阵纠正为当前真实测试落点与 CTest 名称。
- 在 `common-risk-notes.md` 中补充 T060 仍然只是 test-only contract、尚未完成生产 registry -> placement 接线的风险说明。

## health-aware placement 覆盖场景

- `ReadOnly` / `Draining` / stale heartbeat 降级后的 `Unavailable` 节点不会被选为写副本。
- `write_admission_overloaded = true` 的节点会被跳过。
- `disk_pressure = High / Full` 的节点会被跳过，`Medium` 仍可参与稳定排序。
- `available_capacity_bytes` 不满足 `chunk_size + reserve_capacity_bytes` 的节点会被跳过。
- healthy、容量足够、负载较低的节点会按稳定顺序优先。
- 混合候选集中 duplicate `node_id` 不会重复进入多副本结果。
- 全部候选都不合格时返回明确的 `kNodeUnavailable`。
- manager 侧测试展示 placement 只消费 StorageNode facts 和由 heartbeat liveness 推导出的可写性，不写 chunk、不调用 metadata、不接 Raft。

## test-only registry facts 与生产 registry 当前边界

- 本任务只在测试中构造 registry facts 和 liveness 推导 helper。
- 没有新增生产 `StorageNodeRegistry`。
- 没有新增 proto、service/client heartbeat，也没有把 registry facts 接入真实 `PlacementManager` 生产路径。
- T060 固定的是后续 T062/T065 需要满足的 contract，不代表生产接线已完成。

## 是否使用 tests/test_file/test_file.zip

- 否

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_placement_policy|store_placement_manager|health_aware_placement|storage_placement" --output-on-failure 2>&1 | tee tmp/007/t060-health-aware-placement.log`
  - PASS
  - 说明：实际匹配到的测试名为 `store_placement_policy`、`store_placement_manager`
  - 日志路径：`tmp/007/t060-health-aware-placement.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T060 是平台无关 placement contract 测试，没有新增平台相关时间源、文件或网络行为。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- 本任务不新增 `T060-WIN`。

## 是否通过 T060

- 是

## 是否可以进入 T061

- 可以
- 说明：可以继续进入 heartbeat proto/schema 工作，但仍需保留 T059/T060 已固定的 test-only contract 边界，不能把本任务结果误当成生产 registry/placement 接线已完成。

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，执行 `007` 任务时需要人工纠偏。
- 当前 stale heartbeat -> placement eligibility 的适配仍仅存在于测试，生产 clock source、liveness 和排序来源要到 T062/T065 才能真正收口。

## 是否更新 module-notes.md / AGENTS.md

- 否

## module-notes.md 是否需要补充 .cpp 关键函数 / helper

- 否
- 本任务只修改测试和 spec 文档，没有修改生产 `.cpp`。

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T060 标记完成，并把测试文件路径/CTest 名称纠正为当前真实落点。

## common-risk-notes.md 读取结果

- 已读取并维护。
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045、T049、T055、T056、T057、T059 风险继续保留。

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T060` test-only registry facts / health-aware placement contract 与后续生产 registry -> placement 接线风险。
- 删除：无
- 保留：原有风险全部保留
