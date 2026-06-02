# T065 Placement Registry Eligibility

## 修改文件

- `modules/store/placement/placement_manager.h`
- `modules/store/placement/placement_manager.cpp`
- `modules/store/placement/module-notes.md`
- `modules/store/placement/AGENTS.md`
- `tests/store_placement_manager_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t065-placement-registry-eligibility.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `PlacementManager` 中保留原有静态 candidates 入口不变：
  - `SelectPlacement(request, candidates)`
- 新增生产 registry 消费入口：
  - `SelectPlacement(request, registry, now_unix_ms)`
- 新增 snapshot 消费入口：
  - `SelectPlacement(request, registry_snapshot)`
- 在 manager 层补齐 registry snapshot -> placement candidate 的最小协调：
  - 从 `StorageNodeRegistry::Snapshot(now_unix_ms)` 获取节点事实
  - 对 `liveness != Live` 的节点做保守过滤
  - 对 capacity facts 明显不完整/不自洽的节点做保守过滤
  - 将 health、disk pressure、capacity、load、failure-domain 映射到 `StorageNodePlacementCandidate`
  - 再统一委托给既有 `ReplicaPolicySelector::SelectReplicas(...)`
- 扩展 `store_placement_manager_test`，覆盖真实生产 registry facts 的 placement eligibility，以及 snapshot 路径下的 partial facts / duplicate node_id 边界。

## registry facts -> placement candidate 映射语义

- `StorageNodeRegistryNodeSnapshot.node_id` -> `StorageNodePlacementCandidate.node_id`
- `endpoint` -> `endpoint`
- `facts.health.health` -> `health`
- `facts.health.disk_pressure` -> `disk_pressure`
- `facts.capacity.total_capacity_bytes` -> `total_capacity_bytes`
- `facts.capacity.used_capacity_bytes` -> `used_capacity_bytes`
- `facts.capacity.available_capacity_bytes` -> `available_capacity_bytes`
- `facts.load.load` -> `load`
- `facts.load.write_admission_overloaded` -> `write_admission_overloaded`
- `facts.failure_domain.zone/rack` -> `zone/rack`
- `liveness` 不直接映射成 candidate 字段，而是在 manager 侧先做保守过滤：
  - `kLive` 才能进入 selector
  - `kStale` / `kDead` 直接排除，并记录明确原因

## health / liveness / capacity / disk pressure / load eligibility 语义

- manager 侧 registry 专属过滤：
  - `liveness != Live` -> 排除
  - capacity facts 不完整/不自洽 -> 排除
- selector 侧既有写副本 eligibility 继续保持：
  - `health != Healthy` -> 排除
  - `disk_pressure = High / Full` -> 排除
  - `write_admission_overloaded = true` -> 排除
  - `available_capacity_bytes` 无法满足 `chunk_size_bytes + reserve_capacity_bytes` -> 排除
  - duplicate `node_id` 不重复入选
  - `prefer_distinct_zones = true` 时优先跨 zone
  - 排序继续保持确定性：
    - 可用容量更大优先
    - 总 inflight 更低优先
    - active writes 更低优先
    - active reads 更低优先
    - 最后按 `node_id` / 原始顺序稳定排序
- partial facts 当前策略：
  - 对 write placement 必需的 capacity facts，如果 `total/used/available` 不自洽，则保守排除
  - 其它 facts 仍按当前已有默认结构和 selector 语义消费，不在 T065 内扩展 repair/rebalance/read-side 逻辑

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不保存 payload
- T065 只做 registry facts -> placement eligibility 的纯策略接线

## 是否使用 tests/test_file/test_file.zip

- 否

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_placement_policy|store_placement_manager|storage_heartbeat_registry|storage_node_registry" --output-on-failure 2>&1 | tee tmp/007/t065-placement-registry-eligibility.log`
  - PASS
  - 实际匹配到的测试名为 `store_placement_policy`、`store_placement_manager`、`storage_heartbeat_registry`
  - 日志路径：`tmp/007/t065-placement-registry-eligibility.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T065 是平台无关的 registry facts -> placement eligibility 接线任务，一般不单列 `T065-WIN`
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未引入新的平台相关时间源、文件或网络行为

## 是否通过 T065

- 是

## 是否可以进入 T066

- 可以
- 前提：T066 只把 registry facts 接到 read replica selection，不回改 T065 已固定的 write placement 语义

## 当前任务发现的不合理点 / 警告 / 风险

- 任务提示里写的是 `placement_policy.h/.cpp`，但仓库真实生产文件落点是 `replica_policy.h/.cpp`；T065 最终不需要修改 selector 文件，只在 `PlacementManager` 增加 registry/snapshot 协调入口
- `StorageNodeRegistry` 的 freshness 仍依赖调用方提供 `observed_at_unix_ms` 和 snapshot 查询时的 `now_unix_ms`
- failure-domain 目前仍只消费 `zone/rack` 占位字段，没有更细粒度 spread 策略

## 是否更新 module-notes.md / AGENTS.md

- 是
- `modules/store/placement/module-notes.md`：补充 registry snapshot 消费路径和相关 helper/边界
- `modules/store/placement/AGENTS.md`：补充 production registry snapshot 已接 write placement 的模块边界

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - registry snapshot 转 placement candidate helper
  - liveness / capacity facts 过滤 helper
  - failure-domain 映射 helper
  - `PlacementManager` registry eligibility 入口
  - stale/expired facts 过滤 helper

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T065 标记完成，并记录实际影响文件
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：收缩“Placement 未接生产 registry facts”风险，并保留 read-side / freshness / failure-domain 未完备风险

## common-risk-notes.md 读取结果

- 已读取并维护
- Windows durability、GC/recovery、read replica selection、clock/sequence freshness 等风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T065`，记录 write placement 已接生产 registry，但 failure-domain 仍较粗粒度、freshness 仍依赖外部时钟、read-side 仍未接线
- 删除：无整项删除
- 收缩：
  - `T059` 从“Placement/read-side 未接线”收缩为“read-side 未接线”
  - `T060` 从“生产 registry -> PlacementManager 未接线”收缩为“write placement 已接线，但 read-side / failure-domain / overload 最终生产语义未完全收口”
  - `T061` / `T062` 从“placement/read-side 未消费”收缩为“read-side 未消费”
