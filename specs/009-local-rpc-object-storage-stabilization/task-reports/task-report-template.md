# TXXX <Task Name>

> 模板用途：供 009 后续单任务报告复用。此文件只保留模板，不写入具体任务执行结果。

## Report Naming Rule

每个任务报告必须按任务名称命名：

`tXXX-<short-task-name>.md`

要求：

- 使用小写
- 空格替换为 `-`
- 去掉反引号、斜杠、括号等不适合文件名的字符
- 保留任务号前缀
- 不使用泛化的 `phase-xx-*.md` 作为单任务报告名

示例：

- `t006-add-storagenode-first-start-identity-creation-tests.md`
- `t018-add-deterministic-self-refresh-beyond-ttl-test.md`
- `t045-add-run-time-storagenode-registration-test.md`

## Scope

说明本任务边界：

- 任务类型：测试 / 实现 / 文档 / 协议 / example / 验证
- 本任务做什么
- 本任务明确不做什么

## Task Source

- `tasks.md`: TXXX
- 相关 `spec.md` / `plan.md` / `contracts/...` / `validation-matrix.md`
- 如适用，补充 `cross-task-risk-notes.md`、`quickstart.md`、相关 task report

## Files Changed

列出本任务实际修改的文件：

- `path/to/file-a`
- `path/to/file-b`

如果没有修改某类文件，也可以明确写：

- 未修改生产代码
- 未修改测试代码
- 未修改 proto
- 未修改 CMake

## What Changed

简洁说明：

- 做了什么
- 为什么这样做
- 是否只是文档落地、测试补充、实现接线、验证执行

## Boundary Checks

根据任务类型选择填写，不要求每项都出现，但要明确本任务没有越界做什么。

- 没有修改生产代码
- 没有修改测试断言
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 没有把 ViewNode 当成 Raft membership authority
- 没有让 StorageNode join 进入 Raft log
- 保持 committed membership authority 仍由 Raft 决定
- 保持 odd voter invariant

## Validation

记录本任务实际执行的验证。至少说明执行了什么、结果是什么、如果失败日志在哪。

- 构建命令：如执行则填写；未执行写 `Not run`
- 测试命令：如执行则填写；未执行写 `Not run`
- 脚本命令：如执行则填写；未执行写 `Not run`
- 文件存在性检查：文档任务至少填写
- 结果：`PASS` / `FAIL` / `SKIPPED`
- 失败摘要：失败测试名、关键断言、失败分类、最后 50 行日志路径或摘要
- 完整日志路径：如有

文档任务可参考：

```bash
test -f specs/009-local-rpc-object-storage-stabilization/tasks.md
test -f specs/009-local-rpc-object-storage-stabilization/task-reports/<report-file>.md
```

## Build Lock

如果涉及 build/test，必须记录：

- 是否使用 `flock` 构建锁
- 是否获得锁
- 如果未获得锁，说明 build/test skipped

如果本任务不需要 build/test，写：

- `Not required for this documentation-only task.`

## Platform Notes

必须明确平台状态，不伪造 Windows PASS。

- Linux：已验证 / documentation-only / skipped
- Windows：实测结果，或 `pending`
- macOS：实测结果，或 `pending`

## Risks / Follow-ups

记录：

- 本任务发现的风险
- 未完成项
- 需要后续任务处理的问题
- 如果是跨任务风险，是否已同步到 `cross-task-risk-notes.md`

## Result

- 最终状态：`PASS` / `PARTIAL` / `BLOCKED`
- 是否可以进入下一任务
- 如果不能进入下一任务，阻塞原因是什么
