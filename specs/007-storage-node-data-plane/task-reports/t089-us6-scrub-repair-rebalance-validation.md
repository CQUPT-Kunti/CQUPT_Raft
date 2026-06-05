# T089 US6 Scrub Repair Rebalance Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t089-us6-scrub-repair-rebalance-validation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 运行了哪些 US6 验证

- `cmake --build --preset debug-ninja-low-parallel`
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "storage_scrub_repair|storage_rebalance" --output-on-failure`
- 实际命中测试：
  - `storage_scrub_repair`
  - `storage_rebalance`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "storage_scrub_repair|storage_rebalance" --output-on-failure 2>&1 | tee tmp/007/t089-us6-scrub-repair-rebalance.log`
  - PASS
- 日志路径：`tmp/007/t089-us6-scrub-repair-rebalance.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证为 PASS，无失败项。

## Windows 验证判断

- T089 只覆盖当前 Linux 环境下的 US6 低并发验证。
- 当前无 Windows 编译环境，不宣称 Windows PASS。
- 既有 Windows 文件语义待验证风险继续保留。

## 是否通过 T089

- 是。

## 是否可以进入 T090

- 可以。

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，与当前 007 任务不一致；本次按 007 实际任务与测试入口继续执行。
- 本次仅做 US6 低并发验证，不覆盖 T090 之后的 no-KV audit、全量并发、恢复/持久化和全量回归。

## 是否修改高频文档及原因

- 已修改 `specs/007-storage-node-data-plane/tasks.md`，将 T089 标记完成并记录本次验证结果。

## common-risk-notes.md 读取结果

- 已读取 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`。
- 本次验证未发现新风险，也未解决既有风险。

## common-risk-notes.md 新增/删除/保留情况

- 新增：无。
- 删除：无。
- 保留：真实 metadata manifest coordination / Raft 提交未实现、RepairManager / RebalanceManager persistence 未实现、read-side repair 未实现、RebalanceManager 自动后台调度未实现、Windows 文件语义待验证、metadata / registry facts 新鲜度风险、repair/rebalance 后 manifest 更新与 cleanup 的一致性风险。
