# T028 任务报告

## 1. 修改了哪些文件

- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t028-checksum-mismatch-download-test.md`

## 2. T028 的 checksum mismatch 下载失败测试做了什么

- 在 `tests/integrated_object_storage_e2e_test.cpp` 新增 `IntegratedObjectStorageE2ETest.ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture`。
- 该 enabled 用例通过真实 `MetadataStateMachine` 路径创建 bucket、创建对象、提交 COMMITTED manifest，并在 manifest 中记录基于原始 payload 计算的 chunk checksum 与 object checksum。
- 测试随后构造损坏后的 chunk 数据文件，验证：
  - committed manifest 中的第一段 healthy chunk checksum 仍与原始数据匹配
  - 损坏 chunk 的实际 checksum 与 manifest 声明值不匹配
  - 损坏对象整体 checksum 也与原始对象 checksum 不匹配
- 这为后续真实下载实现锁定了 fail-fast 前置条件：download 路径必须依据 manifest checksum 显式失败，不能静默拼接损坏数据。
- 同时新增 `IntegratedObjectStorageE2ETest.DISABLED_ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile`，保留后续真实 download 接入后的最终失败语义入口，要求 checksum mismatch 时不得把损坏文件当作成功输出。

## 3. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

- 有。
- `DISABLED_ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile` 是明确的 disabled/scaffold 测试。
- 原因：当前仓库尚未完成真实的 manifest-driven download、MetadataTransferClient、StorageTransferClient、storage_client download 路径，无法在不引入生产实现的前提下执行真正的端到端失败流程。
- 后续启用条件：
  - `T032` metadata transfer adapter
  - `T034` storage transfer adapter
  - `T035` ViewNode discovery 接入
  - `T036` manifest-driven download reconstruction 与 checksum fail-fast
  - `T037` `storage_client upload/download`

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 enabled 用例锁定的是 checksum mismatch 的前置条件和失败契约，不是完整 download 执行链；真正“失败后不发布损坏文件”的行为仍需后续 transfer 实现接入后由 disabled 用例转正验证。
- `specs/008-integrated-object-storage-system/tasks.md` 在本任务开始前已经存在 T026/T027/T029/T031/T033 的未提交勾选变更；本任务只额外把 T028 从 `[ ]` 改为 `[X]`。
- 用户示例中的构建 target 名是 `integrated_object_storage_e2e`，但仓库当前实际 target 为 `test_integrated_object_storage_e2e`。
- `ctest --preset debug-tests` 仍可能因 preset 指向 `build/linux` 而不是 `build/linux/safe` 返回 `No tests were found!!!`；若发生，需要显式使用 `--test-dir build/linux/safe`。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md；如未修改，明确说明未修改

- 未修改 `common-risk-notes.md`。
- 未修改 `risk-register.md`。

## 6. 验证命令和结果

- `git diff -- tests/integrated_object_storage_e2e_test.cpp tests/CMakeLists.txt tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t028-checksum-mismatch-download-test.md`
  - 结果：确认本任务只改动了测试文件、T028 勾选和任务报告；`tests/CMakeLists.txt`、`tests/support/integrated_cluster_test_utils.h` 无本任务改动。
- `flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_e2e'`
  - 结果：PASS，成功完成 configure，并成功编译 `test_integrated_object_storage_e2e`。
- `flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R \"IntegratedObjectStorageE2ETest\\.ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture\" --output-on-failure'`
  - 结果：命中已知目录不一致问题，CTest 在 `build/linux` 下返回 `No tests were found!!!`，不视为用例失败。
- 如 `debug-tests` preset 因目录不一致返回 `No tests were found!!!`，补充执行：
  - `flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R \"IntegratedObjectStorageE2ETest\\.ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture\" --output-on-failure'`
  - 结果：PASS，`IntegratedObjectStorageE2ETest.ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture` 单测通过，`0 tests failed out of 1`。
