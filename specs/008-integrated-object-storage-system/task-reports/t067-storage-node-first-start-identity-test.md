# T067 StorageNode first-start identity allocation 测试报告

## 1. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t067-storage-node-first-start-identity-test.md`

未修改：

- `tests/CMakeLists.txt`
- 生产代码
- `proto/`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T067 的 StorageNode first-start identity allocation 测试做了什么

- 新增 `NodeIdentityTest.T067StorageNodeFirstStartCreatesStableIdentityAndReloadsIt`。
- 测试用空 `data_dir` 走真实 `LoadOrCreateNodeIdentity(...)` 路径，验证首次启动会创建 `node.identity`。
- 断言创建结果可诊断：
  - `created_new == true`
  - `loaded_existing == false`
  - `identity_path` 已落盘
  - `diagnostic` 非空
- 随后再走 `LoadNodeIdentity(...)`，验证新创建的 StorageNode identity 可以被重新加载，并保持 `node_id`、`cluster_id`、`node_type`、`source` 稳定。

## 3. 是否发现不合理点 / 警告 / 风险

- 当前测试覆盖的是 durable identity helper 边界，不涉及 app startup；`storage_node_app` 中首次启动装配仍属于后续 T071。
- 仓库里的实际测试 target 名为 `test_node_identity`，不是任务文字中的 `node_identity_test`；本次未改 CMake，只在验证时按实际 target 执行。

## 4. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 5. 验证命令和结果

执行命令：

```bash
git diff -- tests/node_identity_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t067-storage-node-first-start-identity-test.md
git diff --check -- tests/node_identity_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t067-storage-node-first-start-identity-test.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/tests/test_node_identity --gtest_brief=1'
```

结果：

- `git diff`：PASS。改动限定在测试文件、`tasks.md` 和任务报告。
- `git diff --check`：PASS。未发现空白错误。
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity`：PASS。
- `./build/linux/safe/tests/test_node_identity --gtest_brief=1`：PASS，`12` 个 `NodeIdentityTest` 用例全部通过，其中包含 `T067StorageNodeFirstStartCreatesStableIdentityAndReloadsIt`。
