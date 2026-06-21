# T008-A Repair-B Replacement Placement Report

## 任务目标

在不执行真实 repair、复制、写副本或 manifest 更新的前提下，为 repair Decision B 建立 replacement placement 与 exclusion 规则：

- retained healthy manifest replicas 保留为 source facts
- replacement target 只能从当前健康、可写、容量足够且未承载该 chunk 的节点里重新选择
- 原缺失/损坏节点不能因为曾在 manifest 中而被强制补回
- replacement placement 失败时保留明确 diagnostics

## 修改文件

- `modules/store/maintenance/repair_manager.cpp`
- `modules/store/placement/module-notes.md`
- `tests/store_placement_manager_test.cpp`
- `tests/storage_scrub_repair_test.cpp`

## 实现说明

### 1. replacement placement 继续复用现有 placement 边界

本次没有新增公开核心 struct，也没有改 placement 排序语义。

repair target 仍通过：

- `PlacementManager`
- `PlacementRequest`
- `ReplicaPolicySelector`
- `PlacementDecision`

来完成选择。

`repair_manager.cpp` 中的 replacement target 选择继续把 committed manifest 的
`replica_nodes` 整体作为 `excluded_nodes` 传给 placement，所以：

- retained healthy replicas 不会被重新选成 target
- 原 manifest 中坏掉、missing 或当前不适合承载的旧节点也不会被“强制补回”

### 2. replacement target 只依据当前资源事实重选

replacement placement 仍然沿用现有资源优先策略：

- writable health
- available capacity / reserve
- disk pressure
- write admission overload
- inflight / load
- failure domain
- chunk-scoped deterministic jitter

因此不会退回到：

- node_id 优先
- 节点名优先
- endpoint/config 顺序优先
- 固定前 N 个节点

### 3. no-target failure 改为保留 replacement diagnostics

当 placement 失败或没有 candidate 时，repair manager 现在会返回更明确的 detail，包含：

- chunk identity
- retained healthy replicas
- existing manifest replicas
- bad replicas
- decision epoch
- placement exclusions
- placement 原始 error

这让后续 repair-B / T008-B 可以直接消费现有排除事实，而不是只拿到一个泛化的
`no healthy repair target is available`。

### 4. 本阶段仍然只产生日志化决策事实

本次没有实现：

- `ReadChunk`
- `WriteChunk`
- chunk copy
- checksum verify repair loop
- metadata manifest update
- 后台 repair task

新增测试专门确认 submit/planning 阶段不会触发 source read 或 target write。

## 测试覆盖

### placement manager

- `ReplacementPlacementExcludesExistingReplicasAndSelectsBestRemainingTarget`
  - 验证 retained healthy replica 与原坏节点作为显式排除项不会被重选
  - 验证 replacement target 从剩余健康、可写节点里按资源优先选出
  - 验证 read-only / overloaded 继续保留明确 exclusion reason

### repair manager

- `ProductionRepairManagerUnderReplicatedSubmitReportsLostAndNoTarget`
  - 验证 no-target 场景返回 `kNoHealthyTarget`
  - 验证 error detail 包含 chunk id、已有 replica、以及被排除节点原因

- `ProductionRepairManagerCreatesTaskFromCandidateAndRecordsPlan`
  - 验证 task 记录 existing manifest replicas 与 bad replicas
  - 验证 target 不会落在 existing manifest replicas 里

- `ProductionRepairManagerPlanningDoesNotExecuteRepairIoDuringSubmit`
  - 验证 submit/planning 阶段不执行 source read / target write

- `ProductionRepairManagerReplacementPlanningDoesNotForceOriginalReplicaBack`
  - 验证原坏节点即便仍在 manifest 中，也不会被强制重新选为 replacement target

## tasks.md 状态

- 本阶段未新增 `T008-A` 到 `tasks.md`
- 本阶段未勾选原始 `T008`

## 验证

实际执行：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_store_placement_manager test_storage_scrub_repair ) 9>/tmp/cqupt_store_build.lock
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_storage_scrub_repair ) 9>/tmp/cqupt_store_build.lock
ctest --test-dir build/linux --output-on-failure -R "(store_placement_manager|store_placement_policy|storage_scrub_repair)"
```

结果：

- `store_placement_manager`: PASS
- `store_placement_policy`: PASS
- `storage_scrub_repair`: PASS

日志：

- `tmp/test-logs/t008a_verify/ctest_t008a_targeted.log`

## 后续边界

T008-A 完成后，T008-B 还需要继续完成：

- repair-B 如何消费这些 replacement diagnostics 并形成后续执行决策
- 真正的 repair copy / durable write / completion semantics
- repair facts 与 manifest/maintenance 的后续衔接
- 更完整的 repair result reporting 与收敛路径
