# T074 Rename Before Directory Sync Contract

## 修改文件

- `tests/storage_cross_platform_durability_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t074-rename-before-directory-sync-contract.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `storage_cross_platform_durability` matrix 中新增两行 contract：
  - `linux-crash-after-rename-before-parent-directory-sync-contract`
  - `windows-crash-after-rename-before-parent-directory-sync-contract`
- 扩展 Linux 当前环境下的 runtime 测试，固定以下语义：
  - publish/rename 成功后 final 文件已经可见
  - 但在 parent directory sync 之前，不能把 crash 后 durable contract 视为已经完成
  - 只有 `SyncDirectory()` 返回成功且 `durable_boundary_reached=true`，才算收口到完整 durable-after-crash contract
- 没有修改生产代码，没有修改 metadata / Raft，也没有扩成真实断电测试。

## crash after rename before parent directory sync 覆盖场景

- Linux publish 后 final 文件可见
- Linux parent directory sync 成功路径
- rename 已发生但 directory sync 未完成时，contract 明确为“不能宣称无条件 durable”
- required durability operation 不能 silent no-op success
- Windows contract-only row 明确覆盖 rename/publish 后到 parent directory durability 之间的 weaker / unsupported / deferred 边界
- 非 Windows 环境下 Windows case 只做 deferred / contract-only，不伪造 PASS
- `LocalDiskChunkStore::WriteChunk()` 在 required directory sync 未完成时，不把写入当成 durable success 的既有 contract 继续保留

## Linux 已验证项

- `LinuxDurableFile` publish/rename 后 final 文件可见
- `LinuxDurableFile` parent directory sync 成功后达到完整 durable boundary
- rename 后、directory sync 前不等于“crash 后 durable 已完成”
- required durability operation 的 false-positive success 不被接受

## Windows contract-only / 待验证项

- `MoveFileEx` / replace-existing publish 后到 parent directory durability 的 runtime 行为：当前 Linux 环境 deferred
- Windows parent directory durability 是否 supported / weaker / unsupported：当前 Linux 环境 contract-only
- Windows sharing violation、handle 语义、真实 crash seam：当前 Linux 环境 deferred
- 本任务没有伪造 Windows PASS；真实验证仍待 `T014-WIN`、`T023-WIN`、T075 或后续 Windows 实机任务

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_cross_platform_durability|cross_platform_durability|store_durable_file" --output-on-failure 2>&1 | tee tmp/007/t074-rename-before-directory-sync.log`
  - PASS
  - 实际匹配到的测试名为 `store_durable_file`、`storage_cross_platform_durability`
  - 日志路径：`tmp/007/t074-rename-before-directory-sync.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- T074 在本轮只把 Windows contract 表达为 deferred / contract-only
- Windows rename / directory durability / sharing violation 的实机 crash seam 仍待验证

## 是否通过 T074

- 是

## 是否可以进入 T075

- 可以
- T075 继续覆盖 Windows long path / UTF-8 path / permission denied / disk full 错误分类，不要把 T074 扩成 Windows runtime 验证

## 当前任务发现的不合理点 / 警告 / 风险

- T074 只是 contract 测试收口，不是 `kill -9` / 断电级 durability 证明。
- Linux 当前测试能证明“目录同步是 required boundary”，不能单独证明所有文件系统在真实断电下都等价。
- Windows 仍只有 contract-only / deferred，不能把矩阵存在本身当成 Windows runtime 已通过。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T074 完成并记录实际验收范围
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：收缩已由 T074 固定的 crash seam 测试缺口，并保留 Windows / 真实断电级风险

## common-risk-notes.md 读取结果

- 已读取
- Windows durability / publish / sharing violation 实机验证风险仍保留
- 真实断电级 crash 验证风险仍保留
- prerequisites 脚本误指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T074`，记录本轮只固定 rename 后、parent directory sync 前的 contract 测试，不代表真实断电级或 Windows runtime 已验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T068/T069/T070/T071/T072/T073` 等后续风险继续保留
- 收缩：
  - `T071/T072/T073` 中仍指向 T074 的 crash window 表述已收缩到后续 Windows / 真实断电级验证
