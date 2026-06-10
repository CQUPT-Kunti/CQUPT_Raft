# T069 identity/config mismatch failure 测试报告

## 1. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t069-identity-config-mismatch-test.md`

未修改：

- `tests/CMakeLists.txt`
- 生产代码
- `proto/`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T069 的 identity/config mismatch failure 测试做了什么

- 新增 `NodeIdentityTest.T069StorageNodeIdentityMismatchFailsAndKeepsExistingIdentity`。
- 先在 `data_dir` 中创建一份合法 StorageNode identity。
- 然后用不同的 `cluster_id` 和不同的 `node_id` 再次发起 `LoadOrCreateNodeIdentity(...)`，模拟本地 `node.identity` 与当前 cluster config 不匹配的启动场景。
- 断言 mismatch 结果：
  - 返回 `NodeIdentityStatusCode::kConflict`
  - `created_new == false`
  - `loaded_existing == false`
  - `validation` 中包含 `kClusterIdMismatch`、`kNodeIdMismatch`
  - `diagnostic` 含有 `expected`，便于后续 T075 收紧详细诊断
- 最后重新加载原 identity，确认旧文件仍然保持不变，没有被覆盖或替换。

## 3. 是否发现不合理点 / 警告 / 风险

- 该测试当前直接锁定 durable identity helper 的 mismatch fail-fast 行为；更细粒度的 message 文本稳定性仍由 T075 的实现诊断增强配合保证。
- mismatch 场景这里同时覆盖 `cluster_id` 与 `node_id` 两类冲突，能更明确地防止“错误 data_dir 复用后继续启动”的路径。

## 4. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 5. 验证命令和结果

执行命令：

```bash
git diff -- tests/node_identity_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t069-identity-config-mismatch-test.md
git diff --check -- tests/node_identity_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t069-identity-config-mismatch-test.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/tests/test_node_identity --gtest_brief=1'
```

结果：

- `git diff`：PASS。改动限定在测试文件、`tasks.md` 和任务报告。
- `git diff --check`：PASS。未发现空白错误。
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity`：PASS。
- `./build/linux/safe/tests/test_node_identity --gtest_brief=1`：PASS，`12` 个 `NodeIdentityTest` 用例全部通过，其中包含 `T069StorageNodeIdentityMismatchFailsAndKeepsExistingIdentity`。
