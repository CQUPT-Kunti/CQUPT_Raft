# T096 Ensure Example Logs Written Under An Ignored Local Log Path

## Scope

- 任务类型：example 脚本日志路径整理 / ignore 验证 / 文档
- 主体对象：`examples/object-storage-local-009-dynamic/`
- 本任务不修改生产代码，不修改测试断言，不把完整大日志写入报告。

## Task Source

- `tasks.md`: `T096`
- 原始任务文本提到 `phase-10-local-rpc-example.md`
- 按当前项目执行规则，本任务主报告写入：
  - `specs/009-local-rpc-object-storage-stabilization/task-reports/t096-ensure-example-logs-written-under-ignored-local-log-path.md`

## Current Log / PID / Tmp Paths

- 进程日志目录：
  - `examples/object-storage-local-009-dynamic/logs/`
- pid 文件目录：
  - `examples/object-storage-local-009-dynamic/pids/`
- 命令验证日志目录：
  - `tmp/test-logs/`

当前 `009` sibling example 已按上述路径输出，无需额外改脚本或 `.gitignore`。

## Per-Process Log Naming

- ViewNode：
  - `view-1.log`
  - `view-2.log`
- 初始 Metadata voters：
  - `meta-1.log`
  - `meta-2.log`
  - `meta-3.log`
- 初始 StorageNodes：
  - `store-1.log`
  - `store-2.log`
  - `store-3.log`
  - `store-4.log`
  - `store-5.log`
  - `store-6.log`
- 运行中动态节点在对应任务执行时继续复用同一命名约定：
  - `store-7.log`
  - `meta-4.log`
  - `meta-5.log`
- failover 临时 client config：
  - `examples/object-storage-local-009-dynamic/logs/failover-view-2-client.json`

## Command Output Traceability

- T090-T095 的 startup / shutdown / status / roundtrip / join / promote / failover 摘要日志均保存在 `tmp/test-logs/`
- 本任务直接使用的验证工件包括：
  - `tmp/test-logs/t096-check-ignore.log`
  - `tmp/test-logs/t096-git-status-ignored.log`
  - `tmp/test-logs/t096-status.log`
  - `tmp/test-logs/t096-cleanup.log`
  - `tmp/test-logs/t096-summary.log`

报告只记录这些日志文件路径、关键摘要和必要结论，不粘贴完整大日志。

## Ignore Validation

- `git check-ignore -v tmp/test-logs/local-rpc-009/`
  - 命中：`.gitignore:6:/tmp/test-logs`
- `git check-ignore -v examples/object-storage-local-009-dynamic/logs/`
  - 命中：`.gitignore:12:/examples`
- `git check-ignore -v examples/object-storage-local-009-dynamic/pids/`
  - 命中：`.gitignore:12:/examples`
- `git check-ignore -v examples/object-storage-local-009-dynamic/tmp/`
  - 命中：`.gitignore:12:/examples`

`git status --short --ignored --untracked-files=all` 的过滤结果显示：

- `examples/object-storage-local-009-dynamic/logs/*.log` 为 ignored
- `examples/object-storage-local-009-dynamic/pids/*` 为 ignored
- `examples/object-storage-local-009-dynamic/downloads/`、`nodes/` 等 runtime 目录为 ignored
- `tmp/test-logs/t09x-*.log` 为 ignored

结论：本地日志、pid、runtime tmp 输出不会作为 tracked 文件误提交。

## Smoke Validation

- 执行命令：

```bash
examples/object-storage-local-009-dynamic/qidong.sh
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/tingzhi.sh
```

- Linux 结果：`PASS`
- 关键摘要：
  - `log_dir_exists=yes`
  - `pid_dir_exists=yes`
  - `status OK ... target_endpoint=127.0.0.1:8301`
  - `view_nodes=2`
  - `metadata_nodes=3`
  - `storage_nodes=6`
  - `leader_hint` 可见
  - `non_authority_boundary ... raft_membership_authority=false object_manifest_authority=false`

这证明最小 startup/status/shutdown 路径已经把：

- 节点进程日志写入本地 ignored 路径
- pid 文件写入本地 ignored 路径
- `status` 输出保留为独立验证日志

## Cleanup

- cleanup 记录：`tmp/test-logs/t096-cleanup.log`
- 已停止：
  - `view-1`
  - `view-2`
  - `meta-1`
  - `meta-2`
  - `meta-3`
  - `store-1` 到 `store-6`
- 未启动的动态节点在 cleanup 中表现为：
  - `meta-4`: `missing_pid_file`
  - `meta-5`: `missing_pid_file`
  - `store-7`: `missing_pid_file`
- 任务结束后再次检查，无 `009` example 残留进程。

## Platform Notes

- Linux：`PASS`
- Windows：`pending`
- macOS：`pending`

## Result

- 最终状态：`PASS`
- 是否避免把完整大日志写入报告：是
- 是否已满足 T096：是
- 是否已勾选 `T096`：是
- 是否可以进入下一任务：可以，进入 `T097`
