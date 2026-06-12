# T045 Add Run-Time StorageNode Registration Test

## 做了什么

在 `tests/storage_heartbeat_registry_test.cpp` 新增运行时 StorageNode registration 测试，验证 registry 已经存在节点并对外提供查询后，新的 StorageNode 仍然可以在运行过程中注册进来，并被后续 `LookupNode`、`ListNodes`、`Snapshot` 观察到。

本任务只补测试，没有实现生产 dynamic join、heartbeat loop、placement、rebalance 或任何 Raft membership 逻辑。

## 修改文件

- `tests/storage_heartbeat_registry_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t045-add-run-time-storagenode-registration-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 新增测试

- `StorageHeartbeatRegistryTest.RuntimeRegistrationAddsNewNodeToObservedRegistryViews`

## 测试如何验证 run-time StorageNode registration

测试流程：

1. 先构造一个已经运行中的 `StorageNodeRegistry`，注册一个初始 StorageNode。
2. 通过 `ListNodes(120)` 先确认当前查询面只有这一个已存在节点。
3. 在“运行过程中”再注册一个新的 StorageNode，使用不同的 `node_id`、`endpoint`、capacity 和 failure domain facts。
4. 断言注册结果为 `created=true`，且新节点初始 `last_sequence=0`、`liveness=LIVE`。
5. 再通过：
   - `LookupNode(new_node_id, 170)`
   - `ListNodes(170)`
   - `Snapshot(170)`
   验证新节点都已经可见。
6. 同时断言原有节点仍保持原先的 observed state，没有因为运行时新增节点被改写。

当前 `StorageNodeRegistry` API 只暴露 Storage observed-state，不带 Metadata/Raft membership 或 quorum 字段，因此这个测试以“注册后可被 registry 查询面看见”为核心，天然不触碰 Raft log、quorum 或 membership authority。

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_storage_heartbeat_registry > tmp/test-logs/t045-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "^storage_heartbeat_registry$" --output-on-failure > tmp/test-logs/t045-ctest.log 2>&1
```

## 验证结果

- PASS
- build target：`test_storage_heartbeat_registry`
- CTest name：`storage_heartbeat_registry`
- 测试结果：`1/1` 通过
- 日志：
  - `tmp/test-logs/t045-build.log`
  - `tmp/test-logs/t045-ctest.log`

## 结论

- 状态：PASS
- 已满足 T045 勾选条件
- 可以进入后续任务
