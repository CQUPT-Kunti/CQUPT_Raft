# T051 Ensure ViewNode Storage Observed State Merge Feeds Placement Candidate Discovery

## 做了什么

本任务把 ViewNode 合并后的 StorageNode observed state 正式接到了 placement candidate discovery：

1. `PlacementManager` 现在可以直接消费 `ViewNodeRegistry::DiscoverStorage(...)` 的结果。
2. `PlacementManager` 也可以直接对 `ViewNodeRegistry + DiscoverStorageRequest + now_unix_ms` 做 placement。
3. ViewNode `require_writable` 语义收紧为真正的 healthy/writeable 节点，避免把降级或高磁盘压力节点当成正常写入候选。
4. 新的 bridge 只影响后续 write plan 候选发现，不触碰已提交 manifest，也不触碰 Raft membership/quorum。

本任务没有实现旧对象 rebalance，没有把 StorageNode heartbeat 写入 Raft log，也没有修改 Metadata / Raft membership。

## 修改文件

- `modules/view/view_registry.cpp`
- `modules/store/placement/placement_manager.h`
- `modules/store/placement/placement_manager.cpp`
- `tests/view_node_discovery_test.cpp`
- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t051-ensure-viewnode-storage-observed-state-merge-feeds-placement-candidate-discovery.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## ViewNode StorageNode observed state 如何进入 placement candidate discovery

新增了两条 placement 入口：

1. `PlacementManager::SelectPlacement(const PlacementRequest&, const viewdemo::DiscoverStorageResult&)`
2. `PlacementManager::SelectPlacement(const PlacementRequest&, const viewdemo::ViewNodeRegistry&, const viewdemo::DiscoverStorageRequest&, std::uint64_t now_unix_ms)`

数据流是：

1. ViewNode registry 先按既有 incarnation / sequence merge 规则保留最新 Storage observed state。
2. `DiscoverStorage(...)` 返回当前 registry 合并后的 StorageNode snapshot。
3. `PlacementManager` 把 `ViewNodeSnapshot` 转成 `ViewNodeBackedStorageNodeSnapshot`：
   - `node_id`
   - `endpoint`
   - `liveness`
   - `health`
   - `disk_pressure`
   - `capacity`
   - `load`
   - `failure_domain`
   - `observed_time`
   - `sequence`
4. 再复用现有 `SelectPlacement(const ViewNodeBackedStorageNodeSnapshotResult&)` 和 `ReplicaPolicySelector` 做最终 candidate 过滤与选择。

这样 placement 不再依赖手工拼装的 view-backed snapshot，也不会绕过 ViewNode observed state 去用硬编码静态列表。

## 如何过滤不健康、不可写或过期 StorageNode

过滤分两层：

### ViewNode discovery 层

`modules/view/view_registry.cpp` 中 `require_writable=true` 的判定已收紧为：

- `health == Healthy`
- `disk_pressure` 不是 `High` / `Full`
- `total_capacity_bytes > 0`
- `available_capacity_bytes > 0`
- `write_admission_overloaded == false`

这保证 ViewNode 自己在“要求可写节点”场景下不会把降级、只读、摘流、高磁盘压力或过载节点当成正常写入节点。

### placement 层

即使调用方对 `DiscoverStorage` 使用了更宽松的 `live_only=false` / `require_writable=false`，`PlacementManager` 和 `ReplicaPolicySelector` 仍会继续过滤：

- `STALE` / `SUSPECT` / `DEAD`
- `ReadOnly` / `Degraded` / `Unavailable` / `Draining`
- `disk pressure High / Full`
- `write_admission_overloaded`
- 容量无效或容量不足

因此旧 heartbeat、旧 incarnation、过期 observed state 或不健康状态都不会成为最终正常写入候选。

## 是否保持只影响后续 write plan，不修改旧 manifest

是。

`tests/integrated_object_storage_e2e_test.cpp` 中已有的 dynamic placement 集成测试已改成：

- 通过真实 `ViewNodeRegistry` 注册 `store-a/store-b/store-c`
- 在运行中新增 `store-c`
- 再通过新的 placement bridge 生成后续 placement

测试同时继续断言：

- 新加入的 `store-c` 只影响后续对象 placement
- 旧对象 `objects/legacy-before-join.bin` 的 committed manifest/chunk refs 完全不变
- 初始 3 voter quorum 仍保持不变

## 新增或更新的测试

### 1. `tests/view_node_discovery_test.cpp`

新增：

- `ViewNodeDiscoveryTest.PlacementCandidateDiscoveryConsumesMergedObservedStorageState`

覆盖点：

- current incarnation 的 healthy live state 会进入 placement
- old incarnation heartbeat 即使 `observed_time` 更晚，也不会覆盖 current state
- `ReadOnly` 节点不会进入最终 replica 结果
- `High` disk pressure 节点不会进入最终 replica 结果
- `Dead` 节点不会进入最终 replica 结果

### 2. `tests/integrated_object_storage_e2e_test.cpp`

更新：

- `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest`

改为使用真实 `ViewNodeRegistry -> DiscoverStorage -> PlacementManager` 数据流，而不是手工构造 view-backed snapshot。

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e test_storage_heartbeat_registry test_view_node_discovery > tmp/test-logs/t051-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorage|StorageHeartbeatRegistry|ViewNodeDiscovery" --output-on-failure > tmp/test-logs/t051-ctest.log 2>&1
ctest --preset debug-tests -R "storage_heartbeat_registry" --output-on-failure >> tmp/test-logs/t051-ctest.log 2>&1
```

## 验证结果

- PASS
- build：
  - `integrated_object_storage_e2e`
  - `test_storage_heartbeat_registry`
  - `test_view_node_discovery`
- test：
  - `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest` 通过
  - `ViewNodeDiscoveryTest.PlacementCandidateDiscoveryConsumesMergedObservedStorageState` 通过
  - `storage_heartbeat_registry` 通过
  - `ViewNodeDiscovery*` 全组通过
- 说明：
  - 第一条 CTest 正则匹配到了更多 `IntegratedObjectStorage*` 用例和同标签相关用例，但结果全部通过
  - disabled 用例保持 disabled，没有被本任务改动
- 日志：
  - `tmp/test-logs/t051-build.log`
  - `tmp/test-logs/t051-ctest.log`

## 结论

- 状态：PASS
- 已满足 T051 勾选条件
- 可以进入 T052
