# T073 Storage Node Crash Recovery Matrix

## 修改文件

- `tests/storage_node_recovery_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t073-storage-node-crash-recovery-matrix.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `tests/storage_node_recovery_test.cpp` 补充 crash / recovery matrix 用例，用 test-only 本地目录事实模拟不同 crash seam。
- 固定了以下恢复 contract：
  - 只有 staging 的 chunk 重启后不能进入 live index。
  - crash 后只要 canonical live final 文件在本地可见，就按 live 事实恢复。
  - same chunk 的 stale staging 必须先清理，再保留对应 live final。
  - quarantine 事实在重启后仍保持不可读，不会回到 healthy live。
- 没有修改生产代码，没有扩展 Repair / Rebalance / Scrub。

## crash / recovery matrix 覆盖场景

- write staging 后崩溃：
  - fresh staging 文件仍可留在本地，但不进入 live index，不可读
- publish 前崩溃：
  - 只有 staging / partial staging 时，重启后不暴露为 live chunk
- publish 后但 directory sync 前崩溃：
  - 当前测试固定为“只按重启后可见的本地文件事实恢复”
  - 如果 canonical live final 文件在本地可见，则恢复为 live
  - 如果只有 staging / 没有 final，则不恢复为 live
- publish + directory sync 后崩溃：
  - canonical live final 文件可在重启后重建为 live chunk
- stale staging cleanup 与 rebuild 顺序：
  - stale staging 会先被清理，再恢复 live final
  - same chunk 的 stale staging 不会覆盖或干扰 live 恢复
- corrupted / quarantined chunk：
  - quarantined chunk 重启后恢复为 `kQuarantined`
  - 不作为 healthy live，不可读
- malformed / misplaced / duplicate：
  - 继续沿用 T068/T070 已固定的 safe skip / explicit conflict 语义

## staging / live / quarantine / corrupted 当前边界

- staging：
  - fresh staging 可以保留在磁盘上
  - 但不进入 live index，不可读
- live：
  - 只要 canonical live final 文件在重启后的本地目录事实中可见，就可恢复为 `kLive`
- quarantine：
  - `chunks/quarantine/` 中的 canonical 文件恢复为 `kQuarantined`
  - `ReadChunk()` 拒读
- corrupted：
  - 当前纯重启扫描仍不能仅凭 `chunks/live/*.chunk` payload bytes 主动发现“此前未标记”的 live checksum mismatch
  - 这类事实仍依赖 T072 前台 `ReadChunk()` / `StatChunk(verify_checksum=true)` 先触发 quarantine

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata / Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)` 和测试内手写 payload

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_recovery|local_disk_chunk_store|cross_platform_durability|crash_recovery|crash_matrix" --output-on-failure 2>&1 | tee tmp/007/t073-storage-node-crash-recovery.log`
  - PASS
  - 实际匹配到的测试名为 `local_disk_chunk_store`、`storage_node_recovery`、`storage_cross_platform_durability`
  - 日志路径：`tmp/007/t073-storage-node-crash-recovery.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮 crash matrix 只在 Linux 当前环境下验证 test-only 本地文件事实恢复 contract
- Windows publish / rename / directory durability / sharing violation 的 crash matrix 仍待后续任务验证

## 是否通过 T073

- 是

## 是否可以进入 T074

- 可以
- T074 继续固定 crash after rename before parent directory sync 的平台 contract，不要把本轮 test-only matrix 扩成真实断电级 durability 结论

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 crash matrix 是 test-only 文件事实矩阵，不是真实 `kill -9` / 断电测试。
- “publish 后但 directory sync 前崩溃”的 contract 当前只固定为“按重启后实际可见的本地文件事实恢复”，不等于已经证明真实断电级 durability。
- Windows rename/delete/sharing violation / directory durability 仍未实机验证。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只补测试矩阵和任务/风险文档，没有修改 `modules/store/*` 生产代码

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T073 完成，并写明真实测试矩阵范围
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：收缩已被 T073 固定的 crash seam 测试缺口，并保留真实断电级 durability / Windows 后续风险

## common-risk-notes.md 读取结果

- 已读取
- Windows durability / delete / rename / sharing violation 风险仍保留
- 真实断电级 crash、metadata freshness、主动 scrub/repair 风险仍保留
- prerequisites 脚本指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T073`，记录本轮只固定 test-only crash / recovery matrix，不代表真实断电级 durability 或 Windows crash semantics 已验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T068/T069/T070/T071/T072` 等后续风险继续保留
- 收缩：
  - `T071` 从“继续在 T072-T074 中补 crash window 语义”收缩为“继续在 T074 / Windows 验证中补平台差异”
  - `T072` 从“在 T073/T074 中继续补 crash window”收缩为“在 T074 和后续 Windows/scrub 任务中继续补”
