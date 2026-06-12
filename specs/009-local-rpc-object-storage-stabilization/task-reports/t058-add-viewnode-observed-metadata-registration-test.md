# T058 Add ViewNode-Observed Metadata Registration Test

## 做了什么

在 `tests/view_node_discovery_test.cpp` 新增 ViewNode-observed metadata registration 测试，验证动态 MetadataNode 可以被 ViewNode 作为 observed metadata node 记录和发现，但这种 observation 仍然只是 discovery/diagnostic 事实，不是 Raft membership authority。

本任务只补测试，没有实现 `JoinMetadataCluster`、`AddLearner`、learner catch-up、promote 或任何 Raft membership 逻辑。

## 修改文件

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t058-add-viewnode-observed-metadata-registration-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 新增测试

- `ViewNodeDiscoveryTest.MetadataObservedRegistrationRemainsObservationOnlyAndRespectsMergeAndLiveness`

## 测试覆盖了什么

测试分为三段：

1. 先注册一个动态 Metadata candidate，初始 observed `membership_state=joining`。
2. 通过 `LookupNode`、`DiscoverMetadata`、`GetClusterView` 断言：
   - ViewNode 能看到该 metadata node 的 observed facts；
   - 该节点仍然是 `joining`，不会因为被 ViewNode 观察到就自动变成 `learner` 或 `voter`；
   - liveness 初始为 `LIVE`。
3. 再注入一个当前 incarnation 的 heartbeat，然后用一个更旧 incarnation、但更高 sequence 且宣称 `membership_state=voter` 的旧观察去覆盖它，断言该旧状态被 `StaleIgnored` 拒绝，最终保留当前 `joining` observed state。
4. 最后推进时间到 dead TTL 之后，断言：
   - `DiscoverMetadata(live_only=true)` 不再把它当作可用节点返回；
   - `GetClusterView(include_dead_nodes=true)` 仍能看到该 metadata node，但 liveness 为 `DEAD`，且 observed `membership_state` 仍只是 `joining`。

## 如何证明 ViewNode observation 不等于 Raft membership authority

- 测试没有触发任何 Metadata join/propose 路径，只通过 ViewNode registry 的 register/heartbeat 入口写入 observed metadata facts。
- 初始 candidate 被 ViewNode 观察后，返回结果仍保持 `membership_state=joining`，没有被 ViewNode 自动提升成 `learner/voter`。
- 一个旧 incarnation 的“伪 voter”观察即使 sequence 更高、时间更晚，也不能覆盖当前 candidate 状态；说明 ViewNode 只做 observed-state merge，不做 membership authority 判定。
- TTL 过期后该 metadata observation 会被 discovery 排除，只在 cluster view 中以 `DEAD` 诊断状态保留，不会被误当作可用 membership。

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery > tmp/test-logs/t058-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure > tmp/test-logs/t058-ctest.log 2>&1
```

## 验证结果

- PASS
- build target：`test_view_node_discovery`
- CTest regex：`ViewNodeDiscovery`
- 测试结果：`29/29` 通过
- 日志：
  - `tmp/test-logs/t058-build.log`
  - `tmp/test-logs/t058-ctest.log`

## 结论

- 状态：PASS
- 已满足 T058 勾选条件
- 可以进入后续任务
