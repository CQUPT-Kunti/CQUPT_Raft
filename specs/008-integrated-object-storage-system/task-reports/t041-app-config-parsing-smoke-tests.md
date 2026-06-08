# T041 - app config parsing smoke tests

## 1. 修改了哪些文件

- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t041-app-config-parsing-smoke-tests.md`

说明：

- `tests/CMakeLists.txt` 未修改
- `tests/support/integrated_cluster_test_utils.h` 未修改

## 2. T041 的 app config parsing smoke tests 做了什么

本任务在 `tests/integrated_object_storage_e2e_test.cpp` 中新增了一组轻量 smoke tests，用统一 `ClusterConfig` 锁定后续 app parsing 必须满足的启动边界，但不实现真实 app startup：

- 新增测试私有 helper：
  - 生成稳定、平台中立的 `ClusterConfigGenerationRequest`
  - 通过 `GenerateDeterministicClusterConfig(...)` 生成统一 cluster config
  - 在测试文件私有作用域中解析 ViewNode / MetadataNode / StorageNode 的单节点启动视图
  - 在测试文件私有作用域中解析 `storage_client` 所需的最小 cluster bootstrap 视图
- 新增实际 smoke tests：
  - `AppConfigParsingSmokeResolvesViewMetadataStorageAndClientBootstrapFromUnifiedClusterConfig`
  - `AppConfigParsingSmokeRejectsUnknownNodeIdAndRoleMismatchWithClearDiagnostics`
  - `AppConfigParsingSmokeRejectsEndpointAndDataDirConflictsBeforeBootstrap`
  - `AppConfigParsingSmokeRejectsMissingViewDiscoveryForStorageClient`

这些用例只验证“统一 config 是否能提供后续 app 启动需要的边界信息”，不启动真实节点进程，不实现 view / metadata / storage app 的主流程。

## 3. 覆盖了哪些配置解析边界

- ViewNode 可从统一 config 解析：
  - `cluster_id`
  - `node_id`
  - `endpoint`
  - `data_dir`
- MetadataNode 可从统一 config 解析：
  - `cluster_id`
  - `node_id`
  - `endpoint`
  - `data_dir`
  - `snapshot_dir`
  - `raft_id`
  - `initial_role`
- StorageNode 可从统一 config 解析：
  - `cluster_id`
  - `node_id`
  - `endpoint`
  - `data_dir`
- `storage_client` 可从统一 config 解析：
  - `cluster_id`
  - ViewNode discovery endpoints
  - `chunk_policy`
  - `timeouts`
- 明确失败边界：
  - `node_id` 不存在
  - `node_id` 与请求 role 不匹配
  - `endpoint` 冲突
  - `data_dir` 冲突
  - `storage_client` 缺少 ViewNode discovery endpoint

## 4. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

有。

- `DISABLED_AppConfigParsingSmokeCliOverridesMustRespectDurableIdentityAndStartupContracts`

原因：

- 当前任务只做 unified cluster config parsing smoke tests
- 真实 app 层的 `--node_id` / `--data_dir` / `--listen` override 与 durable identity 冲突检查依赖后续任务

后续启用条件：

- `T042` 完成 per-node config resolution
- `T045/T046/T047` 完成 thin app startup
- app 层明确实现 override 的接受/拒绝语义后，再将 scaffold 升级为可执行测试

## 5. 是否发现不合理点 / 警告 / 风险

- 当前仓库尚未提供正式的生产级“按 role + node_id 解析单节点 app 启动视图”接口，因此本任务使用测试私有 helper 锁定验收边界，而不是提前修改生产代码
- `storage_client` 的 smoke test 当前只验证 discovery bootstrap 所需的最小 config 视图，不触发真实 upload/download/status 逻辑，这与 T041 边界一致
- 如果后续 T042/T045-T047 的生产解析语义与这些 smoke helper 不一致，需要以这些 smoke tests 为验收基线进行收敛

## 6. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

未修改。

- `common-risk-notes.md`：未修改
- `risk-register.md`：未修改

## 7. 验证命令和结果

### diff 检查

命令：

```bash
git diff -- tests/integrated_object_storage_e2e_test.cpp \
  tests/CMakeLists.txt \
  tests/support/integrated_cluster_test_utils.h \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t041-app-config-parsing-smoke-tests.md
```

结果：

- `tests/integrated_object_storage_e2e_test.cpp`：本任务新增 T041 smoke helper、4 个实际 smoke tests 和 1 个 disabled scaffold
- `tests/CMakeLists.txt`：本任务未修改；当前 diff 中如出现改动，属于工作区既有未提交变更
- `tests/support/integrated_cluster_test_utils.h`：本任务未修改
- `tasks.md`：本任务将 T041 从 `[ ]` 改为 `[X]`
- 任务报告文件为新增文件

### 最小构建验证

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_e2e' \
|| echo "build lock busy, skip integrated_object_storage_e2e build in this window"
```

结果：

- PASS
- `cmake --preset debug-ninja-safe` 配置成功
- `cmake --build --preset debug-ninja-safe --target integrated_object_storage_e2e` 构建成功
- `test_integrated_object_storage_e2e` 成功链接

### 单独测试验证

实际执行命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "AppConfigParsingSmoke" --output-on-failure' \
|| echo "build/test lock busy, skip T041 smoke test in this window"
```

结果：

- PASS
- 运行了 4 个实际 smoke tests，全部通过
- 1 个测试保持 disabled：
  - `IntegratedObjectStorageE2ETest.AppConfigParsingSmokeCliOverridesMustRespectDurableIdentityAndStartupContracts`
- 说明：
  - 先前尝试 `ctest --preset debug-tests -R "AppConfigParsingSmoke" --output-on-failure` 时返回 `No tests were found!!!`
  - 原因是该 test preset 指向的目录与本次 `debug-ninja-safe` 构建目录不一致
  - 因此改用等价且更精确的 `--test-dir build/linux/safe` 方式完成本任务最小测试验证
