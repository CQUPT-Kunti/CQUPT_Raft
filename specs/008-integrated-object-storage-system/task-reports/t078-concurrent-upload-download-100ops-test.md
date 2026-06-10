# T078 concurrent upload/download 100 operations 测试报告

## 1. 修改了哪些文件

- `tests/integrated_object_storage_concurrency_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t078-concurrent-upload-download-100ops-test.md`

未修改：

- `tests/support/integrated_cluster_test_utils.h`
- `tests/CMakeLists.txt`
- 生产代码
- `proto/`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T078 的 100 operations concurrent upload/download stress test 做了什么

- 新增 `IntegratedObjectStorageConcurrencyTest.T078StressPlanPreparesHundredOperationsWithBoundedResourcesAndSha256Fixtures`
  - 构造 50 个对象的 upload/download 成对操作，共 100 个客户端操作。
  - 每个对象都拥有独立的 `payload`、`object_key`、`request_id`、输入路径和输出路径。
  - 预先写入真实 fixture 文件，并为每个对象计算独立 SHA-256。
  - 断言同一 `object_key` 恰好对应 1 个 upload 和 1 个 download，避免共享 key 造成不可诊断结果。
- 新增 `IntegratedObjectStorageConcurrencyTest.T078OutcomeValidationAcceptsCommittedUploadsOnlyWhenMatchingDownloadVerifiesSha256`
  - 用测试侧 outcome validator 锁定验收边界：
    - 只有 upload 标记为 `committed=true` 才算成功提交。
    - 只有对应 download 标记为 `checksum_verified=true` 才算最终成功。
    - 失败操作必须带明确 failure classification 和失败说明。
- 新增 `IntegratedObjectStorageConcurrencyTest.T078OutcomeValidationRejectsMissingFailureClassificationOrVerifiedDownload`
  - 验证两个关键拒绝条件：
    - download 失败但没有错误分类时，测试必须失败。
    - upload 成功提交但没有匹配的最终 SHA-256 验证 download 时，测试必须失败。
- 新增 disabled acceptance skeleton：
  - `IntegratedObjectStorageConcurrencyTest.DISABLED_T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256`
  - 明确保留未来真正 100-op 并发验收入口，但本任务不越权实现生产 bounded concurrency。

## 3. 如何验证成功 COMMITTED 对象的最终 SHA-256

- 测试在 fixture 准备阶段为每个对象计算独立 `expected_sha256`。
- outcome validator 只把下面这条链路视为成功：
  - upload outcome `committed=true`
  - 存在同 `object_index` 的 download outcome
  - download outcome `checksum_verified=true`
- 如果对象已提交，但没有对应的 checksum-verified download，测试会报错：
  - `committed successfully but has no checksum-verified download`
- 这样可以锁定后续真正启用 100-op 并发验收时的硬标准：
  - 成功 COMMITTED 的对象必须经过最终对象级 SHA-256 验证。

## 4. 如何限制并发资源和避免无界线程/内存

- 测试侧显式把 worker 上限固定为 `8`，作为 bounded concurrency 验收边界。
- 100 个操作只对应 50 个小型 fixture 文件，不创建大文件或整对象常驻内存路径。
- fixture 总大小被断言限制在 `512 KiB` 以内，避免把压力测试写成资源膨胀测试。
- 没有使用长时间 `sleep`、无界线程、无界任务队列或超大 payload。
- 这里只定义测试计划和验收条件；真正的生产并发控制仍留给 T083。

## 5. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

- 有。
- 新增 `IntegratedObjectStorageConcurrencyTest.DISABLED_T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256`
- 原因：
  - 本任务只写并发压力测试，不实现生产 bounded concurrency 控制。
  - 当前 `ObjectTransfer` upload 路径仍停在 `CreateWritePlan` 与 discovery 诊断边界，尚未真正完成 chunk write + `CommitObject` 的 100-op 全链路并发执行条件。
  - T083 才负责生产 bounded concurrency controls。
- 启用条件：
  - upload 主链路能真实完成 chunk write、`CommitObject` 与 manifest 可见性；
  - T083 完成生产 bounded concurrency controls；
  - 届时该 disabled 用例应升级为真正的 50 upload + 50 download 并发 round-trip 验收。

## 6. 是否发现不合理点 / 警告 / 风险

- `tests/CMakeLists.txt` 已经存在 `test_integrated_object_storage_concurrency` 的 guarded target，本次不需要改 CMake。
- 仓库当前没有单独的 `integrated_object_storage_concurrency` 自定义 target，因此本次按“最小相关 target”构建了 `test_integrated_object_storage_concurrency`。
- 当前 `tasks.md` 工作区差异里已经包含 `T077=[X]`；本任务只新增 `T078=[X]`，没有改动 T076/T077/T079 的测试内容。
- 由于真正的 upload 并发提交链路尚未完全具备，本次 active 用例主要锁定：
  - 100-op 压测输入计划
  - SHA-256 验收标准
  - failure classification 约束
  - bounded resource 边界
- 真正跨进程 100-op 并发 round-trip 仍依赖后续 T083 和现有 upload 完整执行路径继续收敛。

## 7. 是否修改 common-risk-notes.md 或 risk-register.md；如未修改，明确说明未修改

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 8. 验证命令和结果；如果 target 尚未接入或构建锁被占用，也要说明

执行命令：

```bash
git diff -- tests/integrated_object_storage_concurrency_test.cpp tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t078-concurrent-upload-download-100ops-test.md
git diff --check -- tests/integrated_object_storage_concurrency_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t078-concurrent-upload-download-100ops-test.md
flock -n /tmp/cqupt_raft_build.lock bash -lc "cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_concurrency"
flock -n /tmp/cqupt_raft_build.lock bash -lc "ctest --test-dir build/linux/safe --output-on-failure -R 'IntegratedObjectStorageConcurrencyTest\\.T078(StressPlan|OutcomeValidation)'"
```

结果：

- `git diff`：PASS。测试源码、任务状态和任务报告已纳入 diff。
- `git diff --check`：PASS。未发现空白错误。
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_concurrency`：PASS。
- `ctest --test-dir build/linux/safe --output-on-failure -R 'IntegratedObjectStorageConcurrencyTest\.T078(StressPlan|OutcomeValidation)'`：PASS，3 个 active T078 用例全部通过。

补充说明：

- 先尝试 `ctest --preset debug-tests` 时，preset 指向的是另一个构建目录，返回 `No tests were found!!!`；随后改为对 `build/linux/safe` 目录执行定向 `ctest`，验证通过。
