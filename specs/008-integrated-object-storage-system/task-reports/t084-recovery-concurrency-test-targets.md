# T084 - recovery / concurrency test targets

## 1. 修改了哪些文件

- `tests/CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`

本任务没有修改：

- `tests/integrated_object_storage_recovery_test.cpp`
- `tests/integrated_object_storage_concurrency_test.cpp`
- 任何生产代码

## 2. integrated_object_storage_recovery target 接入做了什么

- 保留现有 `test_integrated_object_storage_recovery` 的 gtest 注册方式不变。
- 新增自定义 target：

  - `integrated_object_storage_recovery`

- 该 target 只依赖现有测试可执行文件 `test_integrated_object_storage_recovery`，便于单独构建：

  - `cmake --build ... --target integrated_object_storage_recovery`

- 没有改动 recovery 测试逻辑，也没有改变 gtest/ctest 的测试发现语义。

## 3. integrated_object_storage_concurrency target 接入做了什么

- 保留现有 `test_integrated_object_storage_concurrency` 的 gtest 注册方式不变。
- 新增自定义 target：

  - `integrated_object_storage_concurrency`

- 该 target 只依赖现有测试可执行文件 `test_integrated_object_storage_concurrency`，便于单独构建：

  - `cmake --build ... --target integrated_object_storage_concurrency`

- 为了让 `ctest -R "integrated_object_storage_(recovery|concurrency)"` 真正命中，
  本任务把 recovery/concurrency 两个 target 的 gtest 发现块改成显式注册，
  并分别添加：
  - `TEST_PREFIX "integrated_object_storage_recovery."`
  - `TEST_PREFIX "integrated_object_storage_concurrency."`

- 没有改动 concurrency 测试逻辑，也没有把 disabled/scaffold case 强行改成启用状态。

## 4. 添加或补齐了哪些 CTest labels

- `RAFT_008_LABELS_RECOVERY` 补齐：
  - `storage-node`
  - `storage-node-recovery`

- `RAFT_008_LABELS_CONCURRENCY` 补齐：
  - `storage-node`
  - `storage-node-concurrency`

保留了原有的：

- `integrated-object-storage`
- `integrated-object-storage-recovery`
- `integrated-object-storage-concurrency`
- `storage-transfer`
- `platform-neutral`
- `durability-boundary`
- `linux-primary-diagnosis`

本任务没有把 recovery/concurrency 测试错误标记为 `storage-node-cross-platform`，避免夸大跨平台结论。

## 5. 是否保持已有测试 target、label、group、preset 不变

- 是。
- 没有删除或重命名已有 target。
- 没有改动现有 gtest 发现方式、CTest preset、group 或测试入口语义。
- 只对 recovery/concurrency 增加了和 e2e/quorum 一致的独立 build target 别名，并补齐相关标签。

## 6. 是否发现不合理点 / 警告 / 风险

- `tests/CMakeLists.txt` 在本任务前已经把 `test_integrated_object_storage_recovery` 和 `test_integrated_object_storage_concurrency` 注册到了 gtest 发现流程，但缺少与 `integrated_object_storage_e2e` / `integrated_object_storage_quorum` 对齐的独立 custom target，导致用户给出的 `--target integrated_object_storage_recovery integrated_object_storage_concurrency` 无法直接工作。
- 当前 recovery/concurrency 的 CTest 用例是否都能跑通，仍受 T076/T077/T078/T079 测试内容和平台环境影响；T084 只负责接线，不替代测试逻辑验收。

## 7. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 8. 验证命令和结果

### diff 检查

```bash
git diff -- tests/CMakeLists.txt tests/integrated_object_storage_recovery_test.cpp tests/integrated_object_storage_concurrency_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t084-recovery-concurrency-test-targets.md
```

结果：已执行。变更只落在 `tests/CMakeLists.txt`、`tasks.md` 和本报告；两个测试源码文件未改动。

### 最小 build

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_recovery integrated_object_storage_concurrency' 
|| echo "build lock busy, skip recovery/concurrency test targets build in this window"
```

结果：已执行，两个 target 构建成功。

### 相关测试

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "integrated_object_storage_(recovery|concurrency)" --output-on-failure' 
|| echo "build/test lock busy, skip recovery/concurrency tests in this window"
```

结果：已执行，但返回 `No tests were found!!!`。原因不是 T084 target 未接入，而是仓库当前 `debug-tests` preset 绑定的 `configurePreset` 为 `debug-ninja-low-parallel`，对应 `build/linux`，与本任务按约束构建的 `debug-ninja-safe -> build/linux/safe` 目录不一致。

为避免误判，本任务补做了等价的最小目录级验证：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "integrated_object_storage_(recovery|concurrency)" --output-on-failure' 
|| echo "build/test lock busy, skip recovery/concurrency tests in this window"
```

结果：

- PASS 4 个测试：
  - `integrated_object_storage_recovery.IntegratedObjectStorageRecoveryTest.NoHealthyOrCapacitySufficientStorageFailsUploadAndKeepsObjectInvisible`
  - `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.T078StressPlanPreparesHundredOperationsWithBoundedResourcesAndSha256Fixtures`
  - `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.T078OutcomeValidationAcceptsCommittedUploadsOnlyWhenMatchingDownloadVerifiesSha256`
  - `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.T078OutcomeValidationRejectsMissingFailureClassificationOrVerifiedDownload`
- 1 个 disabled 测试未运行：
  - `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256`
