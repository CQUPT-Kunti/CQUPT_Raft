## T053

### 做了什么

- 在 `MetadataTransferClient::GetObjectManifest()` 的成功返回路径补充 committed manifest no-rebalance 边界诊断。
- 在现有动态 StorageNode 加入集成测试上强化回归保护，显式断言新加入节点不会进入旧对象的 committed manifest。

### 修改文件

- `modules/store/transfer/metadata_transfer_client.cpp`
- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t053-add-no-rebalance-invariant-diagnostics-for-committed-object-manifest.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

### 如何保护 committed manifest no-rebalance invariant

- `GetObjectManifest()` 在返回 `COMMITTED` manifest 时，追加一条明确诊断：
  - committed manifest 只来自 metadata committed state
  - dynamic StorageNode discovery 只影响 future placement
  - 现有 manifest replica facts 不能被隐式 rewrite 或 rebalance
- 该诊断不修改 manifest 内容，不引入新的 placement 解释逻辑，也不把 ViewNode observed state 当作 manifest authority。

### 如何确认动态 StorageNode 只影响后续新对象

- 复用 T048 的动态加入场景：
  - 旧对象先提交 committed manifest
  - `store-c` 动态加入后，future placement 能看到 `store-c`
  - 再次读取旧对象 committed manifest，断言与加入前完全一致
  - 额外断言 `store-c` 不会出现在旧对象的任一 chunk `replica_nodes` 中
- 这保证了新 StorageNode 只影响后续 write plan，不会反向改写已提交对象的下载事实来源。

### 新增或更新了哪些测试

- 更新 `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest`
- 新增的断言关注：
  - 旧 manifest 在 join 前后保持一致
  - `store-c` 不会进入旧对象 manifest
  - future placement 仍可看到 `store-c`

### 验证命令和结果

- 构建命令：
  - `(
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e
    ) 9>/tmp/cqupt_raft_build.lock`
  - 结果：PASS
- 测试命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorage" --output-on-failure`
  - 结果：PASS
- 测试日志：
  - `tmp/test-logs/t053-build.log`
  - `tmp/test-logs/t053-ctest.log`

### 结果

- 状态：PASS
- 已在 `tasks.md` 中只勾选 T053 完成。
- 可以进入 T054；本任务没有实现旧对象 rebalance、manifest 重写、Raft membership 变更或 StorageNode join 入 Raft log。
