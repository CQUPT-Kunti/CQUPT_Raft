# T090 Add 2-ViewNode Startup Support

## Scope

- 任务类型：example 启动脚本 / 验证 / 文档
- 本任务只为 009 sibling example 增加 `2` ViewNode 启动支持。
- 本任务不实现 shutdown 脚本、不实现 runtime StorageNode join、不实现 Metadata learner join、不实现 failover roundtrip。

## Files Changed

- `examples/object-storage-local-009-dynamic/qidong.sh`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t090-add-2-viewnode-startup-support.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Startup Script Strategy

- 保持 `examples/object-storage-local-3meta-6store/qidong.sh` 完全不动，继续作为 008 baseline。
- 新增 sibling 009 启动脚本：
  - `examples/object-storage-local-009-dynamic/qidong.sh`
- 脚本直接使用 T089 的：
  - `examples/object-storage-local-009-dynamic/cluster.json`
- 启动顺序固定为：
  - `view-1`, `view-2`
  - `meta-1`, `meta-2`, `meta-3`
  - `store-1` 到 `store-6`

## What The Script Does

- 检查所需二进制是否存在：
  - `view_node_app`
  - `metadata_node_app`
  - `storage_node_app`
- 创建：
  - `examples/object-storage-local-009-dynamic/logs/`
  - `examples/object-storage-local-009-dynamic/pids/`
  - 每个节点自己的 `data_dir`
  - Metadata 的 `snapshot_dir`
- 每个进程写独立日志：
  - `logs/view-1.log`
  - `logs/view-2.log`
  - `logs/meta-1.log` ... `logs/meta-3.log`
  - `logs/store-1.log` ... `logs/store-6.log`
- 每个进程写独立 pid：
  - `pids/view-1.pid`
  - `pids/view-2.pid`
  - `pids/meta-1.pid` ... `pids/meta-3.pid`
  - `pids/store-1.pid` ... `pids/store-6.pid`
- 如果某一步启动失败：
  - 不静默吞掉错误
  - 输出失败节点和日志路径
  - 回滚本次新起的进程

## Boundary Preservation

- 008 baseline：保留
  - 未修改 `examples/object-storage-local-3meta-6store/qidong.sh`
- `2` ViewNodes 只按 T089 config 的 `peer_seeds` 启动，不扩张为任何 Raft authority。
- Metadata 初始角色仍只启动 `3` 个 voter：
  - `meta-1..meta-3`
- 未把 dynamic Metadata learner candidate (`meta-4` / `meta-5`) 预先拉起。
- 未把 dynamic StorageNode (`store-7`) 预先拉起。
- 未把 ViewNode observation 写成 membership authority。

## Validation

- build 命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client ) 9>/tmp/cqupt_raft_build.lock`
- build 结果：`PASS`
- build 日志：
  - `tmp/test-logs/t090-build.log`

## Startup Smoke

- 启动脚本：
  - `examples/object-storage-local-009-dynamic/qidong.sh`
- 首轮执行：
  - `tmp/test-logs/t090-startup.log`
  - 说明：脚本本身返回成功并写出 pid/log，但在当前命令执行环境结束后，后台子进程会被回收，因此不把这轮“命令返回后的 pid 存活性”作为最终判据。
- 最终判定使用同一 shell 会话内 live-check：
  - `tmp/test-logs/t090-startup-rerun.log`
  - `tmp/test-logs/t090-live-status.log`

## Startup Smoke Result

- `2` ViewNodes：`PASS`
  - `view-1` / `view-2` 均成功启动
  - `view-1.log` / `view-2.log` 存在
  - `view-1.pid` / `view-2.pid` 在 2 秒检查点均为 `RUNNING`
- `3` Metadata voters：`PASS`
  - `meta-1..meta-3` 均成功启动
  - 日志中可见 `initial_role=voter`、`initial_voters=3`、`initial_commit_quorum=2`
  - `meta-1.pid` / `meta-2.pid` / `meta-3.pid` 在 2 秒检查点均为 `RUNNING`
- 初始 `6` StorageNodes：`PASS`
  - `store-1..store-6` 均成功启动
  - `store-1.log` 中可见 `storage_node_app OK ... view_endpoint=127.0.0.1:8301`
  - `store-1.pid` 到 `store-6.pid` 在 2 秒检查点均为 `RUNNING`
- log 文件：`PASS`
  - 所有 `view/meta/store` 对应日志文件都已生成
- pid 文件：`PASS`
  - 所有 `view/meta/store` 对应 pid 文件都已生成
- 启动参数缺失：`PASS`
  - `view-1.log` / `view-2.log` 未见启动参数缺失报错
- ViewNode peer seeds：`PASS`
  - `view-1.log` 中可见对 `127.0.0.1:8302` 的 peer sync 重试与恢复
  - `view-2.log` 成功启动并写出 `peer_seed_count=1`

## Cleanup

- 本任务实际运行了 startup，因此已做清理。
- 清理方式：
  - 在同一 shell 会话里按 `pids/*.pid` 手动停止所有进程并删除 pid 文件
- 说明：
  - 这只是本次验证的手动 cleanup，不代表 T091 已完成。
- 清理后状态：
  - `examples/object-storage-local-009-dynamic/pids/` 中无残留 pid 文件

## Relevant Evidence

- `tmp/test-logs/t090-startup-rerun.log`
  - 记录 `view-1`, `view-2`, `meta-1..3`, `store-1..6` 的启动成功输出
- `tmp/test-logs/t090-live-status.log`
  - 记录 2 秒检查点：
    - `view-1:...:RUNNING`
    - `view-2:...:RUNNING`
    - `meta-1..3:...:RUNNING`
    - `store-1..6:...:RUNNING`
- 日志样例：
  - `logs/view-1.log`: peer sync 先遇到 connection refused，随后 recovered
  - `logs/meta-1.log`: `started at 127.0.0.1:8401`, `committed_voter_count=3`, `quorum=2`
  - `logs/store-1.log`: `storage_node_app OK ... view_endpoint=127.0.0.1:8301`

## Result

- 最终状态：`PASS`
- 是否已勾选 `T090`：是
- 是否可以进入 `T091`：可以
