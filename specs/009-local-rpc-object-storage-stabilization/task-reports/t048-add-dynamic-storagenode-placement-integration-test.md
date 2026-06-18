# T048 Add Dynamic StorageNode Placement Integration Test

## 做了什么

在 `tests/integrated_object_storage_e2e_test.cpp` 新增 dynamic StorageNode placement 集成测试，用 metadata 已提交 manifest 和 view-backed placement snapshot 一起验证：

1. 运行中新增的 StorageNode 可以进入后续对象写入的 placement 结果。
2. 这个变化只影响未来写入，不会自动重写已提交旧对象的 manifest。
3. 这条路径不进入 Raft membership，也不改变初始 voter quorum。

本任务只补测试，没有修改 placement、dynamic join、Raft membership 或 rebalance 的生产实现。

## 修改文件

- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t048-add-dynamic-storagenode-placement-integration-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 新增测试

- `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest`

## 测试如何验证 dynamic StorageNode placement

测试流程：

1. 先在 `MetadataStateMachine` 中创建并提交一个旧对象：
   - bucket: `bucket-t048`
   - object: `objects/legacy-before-join.bin`
   - committed manifest 只引用旧的 StorageNode 副本：`store-a`、`store-b`
2. 读取并保存这份旧 manifest，作为后续“不应被自动改写”的基线。
3. 构造只有 `store-a`、`store-b` 两个 LIVE 节点的 view-backed snapshot，并对一个“未来对象”执行 2 副本 placement，确认结果只包含旧节点。
4. 在 expanded snapshot 中模拟运行中新加入 `store-c`，再对另一个“未来对象”执行 3 副本 placement。
5. 断言新的 placement 结果包含 `store-c`，证明新增 StorageNode 已能参与后续写入选点。
6. 再次读取旧对象 committed manifest，断言 chunks 与加入前完全一致，证明旧对象 manifest 没有被自动重写。
7. 额外使用 `ClusterConfig` 的初始 voter membership 和 `ComputeInitialRaftQuorum(...)` 断言 quorum 仍为 3 voters / quorum 2，说明 Storage discovery 变化没有触碰 Raft membership。

## 如何确认旧对象 manifest 不被自动重写

测试在 StorageNode 动态加入前后都读取：

- `HeadObject(bucket-t048, objects/legacy-before-join.bin)`
- `FindChunkRefs(bucket-t048, objects/legacy-before-join.bin)`

并用 `ExpectChunkRefsEqual(...)` 对比原始 committed chunk refs，要求：

- `chunk_id`
- `offset`
- `size`
- `replica_nodes`
- `checksum`

全部保持不变。

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e > tmp/test-logs/t048-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageE2ETest\\.|test_integrated_object_storage_e2e|integrated_object_storage_e2e" --output-on-failure > tmp/test-logs/t048-ctest.log 2>&1
```

## 验证结果

- PASS
- build target：`integrated_object_storage_e2e`
- CTest：
  - 新增用例 `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest` 通过
  - `integrated-object-storage` label 下本组 11 个启用用例全部通过
  - 3 个既有 disabled 用例保持 disabled，未被本任务改动
- 日志：
  - `tmp/test-logs/t048-build.log`
  - `tmp/test-logs/t048-ctest.log`

## 结论

- 状态：PASS
- 已满足 T048 勾选条件
- 可以进入后续任务
