# T068 restart reuses node_id 测试报告

## 1. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t068-restart-reuses-node-id-test.md`

未修改：

- `tests/CMakeLists.txt`
- 生产代码
- `proto/`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T068 的 restart reuses node_id 测试做了什么

- 新增 `NodeIdentityTest.T068RestartReusesExistingStorageNodeIdWithoutSilentOverwrite`。
- 先在同一 `data_dir` 中创建一次 StorageNode identity，再模拟重启重新走 `LoadOrCreateNodeIdentity(...)`。
- 第二次调用故意传入不同的待创建 `node_id`，验证：
  - 返回的是已存在 identity，而不是新分配 identity
  - `loaded_existing == true`
  - `created_new == false`
  - `node_id`、`cluster_id`、`created_at_unix_ms`、`source` 都保持与首次创建一致
- 最后再用 `LoadNodeIdentity(...)` 回读文件，确认磁盘上的 `node.identity` 没有被静默覆盖。

## 3. 是否发现不合理点 / 警告 / 风险

- 该测试锁定了 durable identity helper 的重启复用边界，但不替代后续 app startup 里的 identity 接线验证。
- 当前用例通过“第二次请求携带不同 node_id”来证明不会静默重写，比单纯比较两次加载结果更能暴露错误覆盖问题。

## 4. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 5. 验证命令和结果

执行命令：

```bash
git diff -- tests/node_identity_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t068-restart-reuses-node-id-test.md
git diff --check -- tests/node_identity_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t068-restart-reuses-node-id-test.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/tests/test_node_identity --gtest_brief=1'
```

结果：

- `git diff`：PASS。改动限定在测试文件、`tasks.md` 和任务报告。
- `git diff --check`：PASS。未发现空白错误。
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity`：PASS。
- `./build/linux/safe/tests/test_node_identity --gtest_brief=1`：PASS，`12` 个 `NodeIdentityTest` 用例全部通过，其中包含 `T068RestartReusesExistingStorageNodeIdWithoutSilentOverwrite`。
