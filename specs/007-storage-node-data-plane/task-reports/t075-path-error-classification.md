# T075 Path Error Classification

## 修改文件

- `tests/storage_cross_platform_durability_test.cpp`
- `tests/store_durable_file_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t075-path-error-classification.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 扩展 `store_durable_file` 的 Linux 当前环境测试，覆盖：
  - UTF-8 safe path 可正常打开 staging writer
  - Windows reserved name 在跨平台安全子集下返回 `kPathInvalid`
  - name-too-long / overlong path segment 返回 `kPathInvalid`
  - staging 目录不可写时返回 `kPermissionDenied`
- 扩展 `storage_cross_platform_durability` matrix：
  - 新增 Linux 已验证 row：path invalid / reserved name、permission denied、disk full failure injection、UTF-8 safe path
  - 新增 Windows contract-only / deferred row：permission denied + disk full classification、reserved name path contract
- 扩展 `LocalDiskChunkStore` 的 test-only durable file 注入，固定 `WriteChunk()` 遇到 `kPathInvalid`、`kPermissionDenied`、`kDiskFull` 时：
  - 返回统一映射后的明确状态
  - 不更新 live index
- 没有修改生产 `durable_file` 逻辑，没有修改 metadata / Raft。

## path / permission / disk-full / UTF-8 / long-path 错误分类覆盖场景

- path traversal / absolute escape 返回 `kPathInvalid`
- Windows reserved name 返回 `kPathInvalid`
- Linux UTF-8 safe path 可正常通过路径校验和 staging writer 打开
- overlong path segment / name-too-long 返回 `kPathInvalid`
- Linux staging 目录不可写时返回 `kPermissionDenied`
- disk full 通过 test-only failure injection 固定为 `kDiskFull`
- `LocalDiskChunkStore::WriteChunk()` 遇到 path / permission / disk-full 错误时不更新 live index
- Windows long path / UTF-8 / permission denied / disk full / reserved name / sharing violation 继续以 matrix contract 表达
- unsupported / deferred 行为不允许 silent success

## Linux 已验证项

- `LinuxDurableFile` 拒绝 traversal / absolute escape
- `LinuxDurableFile` 接受 UTF-8 safe path
- `LinuxDurableFile` 对 reserved name 返回 `kPathInvalid`
- `LinuxDurableFile` 对 overlong path segment 返回 `kPathInvalid`
- `LinuxDurableFile` 对不可写 staging 目录返回 `kPermissionDenied`
- `LocalDiskChunkStore::WriteChunk()` 对 `kPathInvalid` / `kPermissionDenied` / `kDiskFull` 返回明确错误且不写 live index

## Windows contract-only / 待验证项

- Windows long path contract：当前 Linux 环境 deferred
- Windows UTF-8 path contract：当前 Linux 环境 deferred
- Windows permission denied / disk full 分类：当前 Linux 环境 contract-only / deferred
- Windows reserved name / sharing violation 分类：当前 Linux 环境 contract-only / deferred
- 本任务没有伪造 Windows PASS；真实验证仍待 `T014-WIN`、`T023-WIN`、`T025-WIN`、`T026-WIN`

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用小 payload 和 test-only failure injection

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_cross_platform_durability|cross_platform_durability|store_durable_file|path_error|durability_error" --output-on-failure 2>&1 | tee tmp/007/t075-path-error-classification.log`
  - PASS
  - 实际匹配到的测试名为 `store_durable_file`、`storage_cross_platform_durability`
  - 日志路径：`tmp/007/t075-path-error-classification.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- 当前无 Windows 编译环境，不伪造 Windows PASS
- T075 只把 Windows long path / UTF-8 / permission denied / disk full / reserved name / sharing violation 固定为 contract-only / deferred
- Windows 实机验证仍待后续任务

## 是否通过 T075

- 是

## 是否可以进入 T076

- 可以
- T076 应继续覆盖 orphan chunk metadata-driven GC 边界，不要把 T075 扩成 Windows runtime 验证或 GC 实现

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 Linux 已验证的是分类 contract，不等于真实磁盘打满或 Windows 运行时都已实机验证。
- Windows sharing violation、long path、UTF-8 path 和 disk-full 仍缺少实机证据。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只补测试和文档，没有修改 `modules/store/io/*` 生产实现

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T075 完成并记录实际修改范围
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T075 风险条目，并保留 Windows runtime 待验证事实

## common-risk-notes.md 读取结果

- 已读取
- Windows durability / publish / delete / sharing violation / long path 实机验证风险仍保留
- 真实断电级 durability 风险仍保留
- metadata freshness、repair/rebalance/scrub、prerequisites 脚本误指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T075`，记录本轮只固定 Linux + contract-only 的路径和错误分类，不代表 Windows runtime 已验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T068/T069/T070/T071/T072/T073/T074` 等后续风险继续保留
- 收缩：
  - `T073/T074` 中仍指向 T075 的表述已收缩到后续 Windows / 真实断电级验证
