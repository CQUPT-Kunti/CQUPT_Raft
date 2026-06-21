# T008-B Repair-Ready Facts Report

## 任务目标

在 T008-A 已建立 replacement placement 和 exclusion rules 的基础上，把 repair Decision B 所需事实真正接到 `RepairManager` 边界上：

- committed manifest 继续作为 source / replica authority
- retained healthy source、bad replicas、excluded nodes、replacement target 与 placement diagnostics 可被 `RepairTask` 持有
- 无 eligible target 或 source authority 违规时，保留明确 failure detail
- 不执行真实 copy / write / checksum verify / manifest update / 后台 repair

## 修改文件

- `modules/store/maintenance/repair_manager.h`
- `modules/store/maintenance/repair_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_scrub_repair_test.cpp`

## 实现说明

### 1. repair manager 现在保留 manifest-authoritative source facts

`RepairTask` 新增并公开保留：

- `healthy_source_replicas`
- `excluded_nodes`
- `replacement_decision`

同时继续保留已有的：

- `chunk_id`
- `existing_replica_nodes`
- `bad_replicas`
- `target_node`

这样 repair-ready decision 不再只靠 `error_detail` 字符串，而是能直接通过已有 repair task 边界读取关键 facts。

### 2. healthy source 必须来自 committed manifest

`SelectSourceNode(...)` 现在显式校验：

- `repair_candidate.healthy_source_replicas` 必须属于 `manifest.replica_nodes`
- manifest 外健康节点会被记录为 exclusion reason：
  - `healthy repair source is not in committed manifest replicas`

因此：

- discovery / scrub / registry 看到的 manifest 外健康节点
  - 可以作为 replacement target 候选
  - 但不能被当成既有 source replica

### 3. replacement target 继续复用 T008-A placement

repair manager 仍然不自己拼 target 排序。

它继续：

- 把 committed manifest replicas 作为 `excluded_nodes`
- 调用 `PlacementManager::SelectPlacement(...)`
- 直接保存 `PlacementDecision`

因此 replacement target 的选择仍然完全来自 T008-A 的资源优先 + deterministic jitter 规则。

### 4. failure / diagnostics 语义补全

source 不可用时，`SelectSourceNode(...)` 会把 source exclusions 拼进 failure detail。

target 不可选时，repair manager 继续返回带以下事实的 placement failure：

- chunk identity
- retained healthy replicas
- existing manifest replicas
- bad replicas
- placement exclusions
- placement error

不再退化成普通 `kInternalError` 或伪造 repair success。

### 5. 本阶段仍不执行真实 repair

本次没有实现：

- source `ReadChunk`
- target `WriteChunk`
- copy / checksum verify
- metadata manifest update
- Raft proposal
- background repair scheduling

`SubmitTask()` / `SubmitUnderReplicatedTask()` 只生成 repair-ready task 与 decision facts。

## 测试覆盖

新增 / 强化：

- `ProductionRepairManagerCreatesTaskFromCandidateAndRecordsPlan`
  - 验证 `RepairTask` 保留 `healthy_source_replicas`
  - 验证 `replacement_decision` 与 selected target 一致
  - 验证 existing manifest replicas / bad replicas 仍被保留

- `ProductionRepairManagerPlanningDoesNotExecuteRepairIoDuringSubmit`
  - 验证 planning 阶段不触发 source read / target write

- `ProductionRepairManagerReplacementPlanningDoesNotForceOriginalReplicaBack`
  - 验证原坏节点不会被强制补回

- `ProductionRepairManagerRejectsManifestExternalHealthySourceAuthorityLeak`
  - 验证 manifest 外健康节点不能被当成既有 source
  - 验证返回明确 authority failure detail

- `ProductionRepairManagerUnderReplicatedSubmitReportsLostAndNoTarget`
  - 验证 no-target failure detail 继续保留 chunk/source/exclusion facts

## tasks.md 状态

- 本阶段没有新增 `T008-B` 到 `tasks.md`
- targeted build/test PASS 后，已将原始 `T008` 从 `[ ]` 更新为 `[X]`

## 验证

实际执行：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_storage_scrub_repair test_store_placement_manager test_store_placement_policy test_metadata_manifest ) 9>/tmp/cqupt_store_build.lock
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_storage_scrub_repair ) 9>/tmp/cqupt_store_build.lock
ctest --test-dir build/linux --output-on-failure -R "(storage_scrub_repair|store_placement_manager|store_placement_policy|MetadataManifestTest\\.)"
```

结果：

- `MetadataManifestTest.MetadataProtoCarriesChunkRefsWithoutPayloadBytes`: PASS
- `MetadataManifestTest.MetadataStateMachineSnapshotRoundTripPreservesChunkRefsWithoutPayloadMarker`: PASS
- `storage_scrub_repair`: PASS
- `store_placement_policy`: PASS
- `store_placement_manager`: PASS

日志：

- `tmp/test-logs/t008b_verify/ctest_t008b_targeted.log`

## 后续边界

T008 完成后，可以进入下一阶段；后续仍需要在真正的 repair-B 执行阶段继续完成：

- copy / durable write / verify 的闭环
- manifest coordination / update
- repair completion 后的 source cleanup / converged state
- 后续更完整的 repair scheduling 与重试策略
