# T007 Add ViewNode First-Start Identity Creation And Restart Reuse Tests

## 做了什么

本任务只新增 `ViewNode` identity lifecycle 测试，不修改生产实现。测试聚焦：

- `identity_file` 首次缺失时，ViewNode first-start 可以正常创建本地持久 identity。
- ViewNode 重启后复用长期 `node_id`，不会被新的 create 请求静默覆盖。
- ViewNode identity 不携带 Metadata-only 的 `raft_id` / voter authority。
- 本任务测试完全基于本地临时目录和现有 `node_identity` API，不依赖其他 ViewNode、StorageNode 或 MetadataNode 心跳。

## 修改了哪个文件

- `tests/node_identity_test.cpp`

## 新增了哪些测试

### `T007ViewNodeFirstStartCreatesPersistentIdentityAndReloadsIt`

覆盖点：

- 使用临时目录和初始不存在的 `node.identity`。
- 通过现有 `LoadOrCreateNodeIdentity` 入口完成 first-start 创建。
- 断言创建成功、`identity_path` 被写入。
- 断言 `cluster_id` 正确。
- 断言 `node_type=view`。
- 断言长期 `node_id` 非空且与期望一致。
- 断言 `raft_id` 为空，避免把 ViewNode 误当成 Metadata / Raft authority。
- 再次通过 `LoadNodeIdentity` 加载，确认 durable identity 可回读。

### `T007ViewNodeRestartReusesStableNodeIdWithoutAuthorityDrift`

覆盖点：

- 先创建第一次 ViewNode identity。
- 再对同一 `data_dir` 执行第二次 `LoadOrCreateNodeIdentity`。
- 断言 restart 走 `loaded_existing`，不是重新创建。
- 断言长期 `node_id`、`cluster_id`、`node_type`、`created_at_unix_ms`、`source` 保持不变。
- 断言新的 `identity_to_create` 不会静默替换已有 ViewNode identity。
- 断言 `raft_id` 仍为空，避免 authority drift。

## incarnation / boot epoch 当前是否已有可测表达

当前 `modules/cluster/node_identity.h` 中的 `NodeIdentity` 只包含长期 durable identity 字段：

- `cluster_id`
- `node_id`
- `node_type`
- optional `raft_id`
- `identity_version`
- `created_at_unix_ms`
- `source`

当前没有可直接断言的 `incarnation` / `boot_epoch` / `persistent_generation` 字段，也没有单独的 process incarnation 返回值。因此本任务没有强行为 ViewNode 补写 incarnation 断言，而是把该缺口保留给后续：

- `T013 Add process incarnation / boot epoch generation boundary`
- ViewNode merge / self-refresh 相关后续任务

## 验证命令和结果

已执行：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

结果：

- PASS

说明：

- 构建日志：`tmp/test-logs/t007-build.log`

按任务建议先尝试执行：

```bash
ctest --preset debug-ninja-low-parallel -R test_node_identity --output-on-failure
```

结果：

- 失败，原因不是测试断言失败，而是当前仓库不存在该 test preset。
- 失败摘要：`CMake Error: No such test preset ... "debug-ninja-low-parallel"`
- 日志：`tmp/test-logs/t007-ctest.log`

随后使用当前仓库实际存在的 test preset 执行等价定向验证：

```bash
ctest --preset debug-tests -R test_node_identity --output-on-failure
```

结果：

- PASS

说明：

- 测试日志：`tmp/test-logs/t007-ctest-debug-tests.log`

## 是否可以进入 T008 / T013

可以。

- 可以进入 `T008`，继续补 Metadata bootstrap voter identity 测试。
- 可以进入 `T013`，补 process incarnation / boot epoch 的实现和可测表达，再回到 ViewNode restart / merge 相关测试补齐 incarnation 语义。
