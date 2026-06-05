# T069 Cross Platform Durability Matrix

## 修改文件

- `tests/storage_cross_platform_durability_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t069-cross-platform-durability-matrix.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_cross_platform_durability_test.cpp`，把 durability contract 收敛成独立 matrix 测试入口 `storage_cross_platform_durability`。
- matrix 测试分三层：
  - 当前平台覆盖分类：区分 Linux 已验证、Windows deferred、平台无关 contract-only
  - `DurableFile` required operation contract：required operation 不能 silent no-op success
  - `LocalDiskChunkStore` integration contract：publish / directory sync 没达到 required durable boundary 时，`WriteChunk` 不能成功写入 live index
- 在 Linux 当前环境下，对 `LinuxDurableFile` 实际验证：
  - `DurableFlushMode::kDataOnly` flush
  - `DurableFlushMode::kDataAndMetadata` flush
  - publish 后文件可见
  - parent directory sync 成功
  - invalid path / missing staging / missing directory 返回明确错误
- 对 Windows 路径新增 runtime matrix case：
  - Windows 下验证 `FlushFileBuffers`、exclusive publish、replace-existing publish contract、directory durability explicit unsupported
  - 非 Windows 环境不伪造 PASS，明确 `GTEST_SKIP` 为 deferred
- 在 `tests/CMakeLists.txt` 中新增 `storage_cross_platform_durability` CTest 入口，并挂到 `storage-node-cross-platform` 标签。
- 更新 `tasks.md`，将 T069 标记为完成。
- 更新 `common-risk-notes.md`，记录 Windows 当前仍是 contract-only / deferred，且 `ReplaceFileW` 没有独立实现路径。

## cross-platform durability matrix 覆盖场景

- Linux `fdatasync` / `fsync` flush contract
- Linux parent directory sync contract
- Linux same-filesystem publish contract
- Linux invalid path / missing staging / missing directory explicit error
- platform-neutral required operation 不允许 silent no-op success
- `LocalDiskChunkStore::WriteChunk()` 不接受 publish / directory sync 的 false-positive success
- Windows `FlushFileBuffers` contract
- Windows `MoveFileExW` exclusive publish contract
- Windows replace-existing publish contract
- Windows long path / UTF-8 path / sharing violation / directory durability 作为 matrix row 列出
- Windows runtime case 在非 Windows 环境明确 deferred，不伪造 PASS

## Linux 已验证项

- `LinuxDurableFile` data-only flush 达到 durable boundary
- `LinuxDurableFile` data-and-metadata flush 达到 durable boundary
- `LinuxDurableFile` publish 后 final 文件可见
- `LinuxDurableFile` parent directory sync 成功
- `LinuxDurableFile` 对 invalid path / missing staging / missing directory 返回明确错误
- `LocalDiskChunkStore` 在 publish / directory sync 未达到 required boundary 时返回失败，不更新 live index

## Windows contract-only / 待验证项

- `FlushFileBuffers` runtime 行为：当前 Linux 环境 deferred
- `MoveFileExW` exclusive publish：当前 Linux 环境 deferred
- replace-existing publish contract：当前 Linux 环境 deferred
- long path / UTF-8 path：当前 Linux 环境 deferred
- sharing violation：当前 Linux 环境 deferred
- directory durability explicit unsupported / weaker contract：当前 Linux 环境 deferred
- 当前没有伪造 Windows PASS；真实运行验证仍依赖 `T014-WIN` / `T023-WIN`

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)` 的小 payload

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_cross_platform_durability|cross_platform_durability|store_durable_file" --output-on-failure 2>&1 | tee tmp/007/t069-cross-platform-durability.log`
  - PASS
  - 实际匹配到的测试名为 `store_durable_file`、`storage_cross_platform_durability`
  - 日志路径：`tmp/007/t069-cross-platform-durability.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T069 涉及 Windows durability contract
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务没有新增 `T069-WIN`
- 原因：Windows runtime 验证已由既有 `T014-WIN`、`T023-WIN` 等任务继续跟踪
- 当前判断：**Windows 待验证**

## 是否通过 T069

- 是

## 是否可以进入 T070

- 可以
- T070 应继续实现生产 `RebuildIndexFromDisk`，不要把 T069 的 test-only contract matrix 扩展成恢复实现

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 Windows publish 生产实现只有 `MoveFileExW` 路径，没有独立 `ReplaceFileW` 路径；本轮只固定 replace-existing publish contract，不宣称两者 runtime 语义已等价验证。
- `WindowsDurableFile::SyncDirectory()` 仍是 explicit unsupported；这符合当前 contract，但不等于 Windows directory durability 已解决。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只新增测试和文档，没有修改 `modules/store/*` 生产逻辑

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T069 完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：记录 Windows durability matrix 当前仍是 contract-only / deferred，不能误判为实机验证完成

## common-risk-notes.md 读取结果

- 已读取并核对现有风险项
- 仍存在且本任务未关闭的风险包括：
  - prerequisites 脚本仍错误指向 006
  - `T014/T023/T025/T026` 等 Windows 实机验证仍待完成
  - restart rebuild / stale staging cleanup / corrupted quarantine 仍未实现
  - timeout/cancellation 运行中传播未实现
  - GC schema migration / 多进程 persistence_root 协议未定义

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T069`，记录 Windows durability matrix 当前只固定 contract，不代表 Windows runtime 已验证
- 删除：
  - 无
- 保留：
  - 既有风险全部保留
