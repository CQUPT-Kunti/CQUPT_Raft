# T026 任务报告：E2E upload/download happy-path 测试骨架

## 1. 修改了哪些文件

- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t026-e2e-upload-download-scaffold.md`

补充说明：

- 未修改 `tests/CMakeLists.txt`，因为 `test_integrated_object_storage_e2e` 目标已经在 `tests/CMakeLists.txt` 中安全接入。
- 未修改 `tests/support/integrated_cluster_test_utils.h`。
- 未修改任何生产代码、proto 或 app 入口。

## 2. T026 的 happy-path 测试骨架做了什么

本次在 `tests/integrated_object_storage_e2e_test.cpp` 中新增了 T026 对应的 happy-path E2E scaffold，分为两层：

### 可运行的轻量 scaffold test

新增：

- `IntegratedObjectStorageE2ETest.HappyPathUploadDownloadScaffoldPreparesRealFileAndChecksumExpectation`

它当前会：

- 生成跨平台的临时测试目录，不依赖固定 `/tmp` 路径字符串。
- 生成一个轻量但真实的二进制输入文件 `fixture.bin`。
- 预留未来下载目标路径 `fixture.download.bin`。
- 使用现有 `storedemo::ComputeChunkChecksum` 计算输入文件的 SHA-256。
- 验证：
  - 输入文件已创建
  - 下载目标路径尚不存在
  - SHA-256 长度与格式边界正确
  - 文件大小与原始 payload 一致
  - `object_key` 命名骨架为 `objects/fixture.bin`

这个测试不执行 upload/download，但把后续 happy-path E2E 所需的：

- 真实输入文件
- 目标下载路径
- 最终 SHA-256 比对入口

都先建立起来了。

### 明确的 disabled round-trip 占位 test

新增：

- `IntegratedObjectStorageE2ETest.DISABLED_HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage`

这个用例是明确的 E2E 占位骨架，内部准备了：

- 真实输入文件
- 期望 SHA-256
- 未来下载输出路径

但当前不会真正执行 integrated object storage 的 upload/download 流程。

## 3. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

有。

当前存在一个明确的 disabled/scaffold 测试：

- `DISABLED_HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage`

原因：

- T026 只允许建立 happy-path E2E 测试入口和后续接入点。
- 当前还不能提前实现或接入：
  - `storage_client upload/download`
  - `object_transfer`
  - `MetadataTransferClient`
  - `StorageTransferClient`
  - ViewNode discovery 到 transfer
  - manifest-driven download reconstruction

后续启用条件：

- T029/T030：`object_transfer` 接口与实现
- T031/T032：metadata transfer adapter
- T033/T034：storage transfer adapter
- T035：ViewNode discovery 接入 transfer
- T036：manifest-driven download reconstruction
- T037：`storage_client upload/download`

在这些能力完成前，disabled test 只作为 E2E happy-path round-trip 的稳定占位点，不会误导为“系统已具备上传下载能力”。

## 4. 是否发现不合理点 / 警告 / 风险

- `ctest --preset debug-tests` 当前默认指向 `build/linux`，而本任务按并发限制使用的是 `debug-ninja-safe`，输出目录为 `build/linux/safe`。
- 因此直接使用 `ctest --preset debug-tests -R ...` 时，第一次得到的是 `No tests were found!!!`，并不是测试本身失败，而是 preset 与本次 safe 构建目录不一致。
- 为了拿到真实的 T026 用例结果，后续改用带锁的最小命令：
  - `ctest --test-dir build/linux/safe -R "<T026测试名>" --output-on-failure`
- `tasks.md` 在本任务开始前已存在其他未提交改动；本任务只新增 T026 的 `[X]` 勾选，没有改动其他任务文件对应实现。

## 5. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。

## 6. 验证命令和结果

执行：

```bash
git diff -- tests/integrated_object_storage_e2e_test.cpp tests/CMakeLists.txt tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t026-e2e-upload-download-scaffold.md
```

结果：PASS。

说明：

- `tests/CMakeLists.txt` 与 `tests/support/integrated_cluster_test_utils.h` 在本任务中未修改。
- diff 可确认 `tests/integrated_object_storage_e2e_test.cpp` 新增了 T026 scaffold。

执行构建：

```bash
flock -n /tmp/cqupt_raft_build.lock -c "cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_e2e" || echo "build lock busy, skip build in this window"
```

结果：PASS。

补充说明：

- 本窗口成功拿到构建锁。
- `test_integrated_object_storage_e2e` 编译通过。

执行测试（按用户建议的 preset 形式）：

```bash
flock -n /tmp/cqupt_raft_build.lock -c "ctest --preset debug-tests -R \"IntegratedObjectStorageE2ETest\\.HappyPathUploadDownloadScaffoldPreparesRealFileAndChecksumExpectation\" --output-on-failure" || echo "build/test lock busy, skip test in this window"
```

结果：

- `No tests were found!!!`

说明：

- 这是因为 `debug-tests` 当前对应的测试目录不是本次 `debug-ninja-safe` 的输出目录，不代表用例失败。

为获得真实结果，执行了最小修正后的带锁命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c "ctest --test-dir build/linux/safe -R \"IntegratedObjectStorageE2ETest\\.HappyPathUploadDownloadScaffoldPreparesRealFileAndChecksumExpectation\" --output-on-failure" || echo "build/test lock busy, skip test in this window"
```

结果：PASS。

测试命令实际通过的用例：

- `IntegratedObjectStorageE2ETest.HappyPathUploadDownloadScaffoldPreparesRealFileAndChecksumExpectation`

耗时：

- 约 `0.92 sec`
