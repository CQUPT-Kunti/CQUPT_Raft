# T092 Recovery Snapshot Catch-up Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t092-recovery-snapshot-catchup-validation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 运行了哪些 recovery / snapshot / catch-up 验证

- `ctest --test-dir build/linux -N -R "recovery|snapshot|catch|restart"`
- `cmake --build --preset debug-ninja-low-parallel`
- `CTEST_PARALLEL_LEVEL=1 ./test.sh --group recovery --skip-configure --skip-build`
- 实际 recovery 入口说明：
  - 通配 `ctest -N -R "recovery|snapshot|catch|restart"` 当前只列出 `storage_node_recovery`
  - 仓库 `test.sh` 已提供更准确的 snapshot / recovery / catch-up 低并发入口：`--group recovery`
  - 该入口内部固定正则 `^(RaftSnapshotRestartTest|RaftSnapshotRecoveryTest|RaftSnapshotCatchupTest)\.`，本次实际命中 15 个测试并全部通过

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
- `ctest --test-dir build/linux -N -R "recovery|snapshot|catch|restart" 2>&1 | tee tmp/007/t092-recovery-list.log`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel 2>&1 | tee tmp/007/t092-build.log`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ./test.sh --group recovery --skip-configure --skip-build 2>&1 | tee tmp/007/t092-recovery-snapshot-catchup.log`
  - PASS
- 日志路径：
  - `tmp/007/t092-recovery-list.log`
  - `tmp/007/t092-build.log`
  - `tmp/007/t092-recovery-snapshot-catchup.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证为 PASS，无失败项

## Windows 验证判断

- T092 是 Linux 当前环境下 recovery / snapshot / catch-up 低并发验证
- 当前无 Windows 编译环境，不宣称 Windows PASS
- 既有 Windows 待验证风险继续保留

## 是否通过 T092

- 是

## 是否可以进入 T093

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，与当前 007 任务不一致；本次继续按 007 实际任务和恢复验证入口执行
- 通配 `ctest -N -R "recovery|snapshot|catch|restart"` 在当前仓库只列出 `storage_node_recovery`，不足以完整覆盖 snapshot / catch-up；因此本次按仓库真实约定切换到 `test.sh --group recovery`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T092 完成并记录本次低并发恢复验证结果

## common-risk-notes.md 读取结果

- 已读取 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- 本次恢复验证未发现新风险，也未解决既有风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：Windows 文件语义待验证、真实 metadata manifest coordination / Raft 提交未实现、RepairManager / RebalanceManager persistence 未实现、read-side repair 未实现、metadata / registry facts 新鲜度风险、repair/rebalance 后 manifest 更新与 cleanup 的一致性风险、timeout / cancellation 运行中传播边界
