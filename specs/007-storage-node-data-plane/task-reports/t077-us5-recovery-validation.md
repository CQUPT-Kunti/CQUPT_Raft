# T077 US5 Recovery Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t077-us5-recovery-validation.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 运行了哪些 US5 验证

- `storage_node_recovery`
- `storage_cross_platform_durability`
- 验证方式为 `CTEST_PARALLEL_LEVEL=1` 的低并发定向 CTest
- 未运行全量回归、US1-US4 全量验证、US6、Windows 测试或 no-KV audit

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "storage_node_recovery|storage_cross_platform_durability" --output-on-failure 2>&1 | tee tmp/007/t077-us5-recovery-validation.log`
  - PASS
  - 实际匹配到的测试名为 `storage_node_recovery`、`storage_cross_platform_durability`
  - 日志路径：`tmp/007/t077-us5-recovery-validation.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T077 仅针对 Linux 当前环境下的 US5 低并发验证
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 既有 Windows 待验证项继续保留

## 是否通过 T077

- 是

## 是否可以进入 T078

- 可以
- T078 应进入 US6 的 ScrubManager 测试，不要把 T077 扩成新的恢复实现或全量回归

## 当前任务发现的不合理点 / 警告 / 风险

- 本轮只是 Linux 当前环境下的定向低并发验证，不等于 Windows 或真实断电级恢复语义已验证。
- `common-risk-notes.md` 中已有的 Windows 删除 / rename / sharing violation / directory durability、真实断电级 durability、metadata fact 新鲜度、delayed retry scheduler 等风险继续存在。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T077 完成并记录本轮仅做验证

## common-risk-notes.md 读取结果

- 已读取
- 未发现新风险
- 未解决可关闭的旧风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - 无
- 删除：
  - 无
- 保留：
  - 既有 Windows 删除 / rename / sharing violation / directory durability 待验证
  - 真实断电级 durability 未验证
  - timeout / cancellation 运行中传播未实现
  - Repair / Rebalance / Scrub 未实现
  - metadata fact 新鲜度风险
  - delayed retry scheduler 未实现
  - GC schema migration / 多进程 `persistence_root` 协议未定义
