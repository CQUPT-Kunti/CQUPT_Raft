# T091 Add Matching Shutdown Support

## Scope

- 任务类型：example shutdown 脚本 / 验证 / 文档
- 本任务只为 T090 的 sibling 009 startup 脚本补配套 shutdown 支持。
- 本任务不实现新的 startup 逻辑，不实现 runtime StorageNode join，不实现 Metadata learner join，不实现 failover roundtrip。

## Files Changed

- `examples/object-storage-local-009-dynamic/tingzhi.sh`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t091-add-matching-shutdown-support.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Shutdown Script Strategy

- 保持 `examples/object-storage-local-3meta-6store/tingzhi.sh` 完全不动，继续作为 008 baseline。
- 新增 sibling 009 shutdown 脚本：
  - `examples/object-storage-local-009-dynamic/tingzhi.sh`
- 脚本与 T090 的：
  - `examples/object-storage-local-009-dynamic/qidong.sh`
  - `examples/object-storage-local-009-dynamic/pids/*.pid`
  - `examples/object-storage-local-009-dynamic/logs/*.log`
  完全配套。

## What The Script Does

- 只按固定节点集合处理 T090 启动的进程：
  - `store-1..store-6`
  - `meta-1..meta-3`
  - `view-1..view-2`
- 停止顺序固定为：
  - `StorageNodes -> MetadataNodes -> ViewNodes`
- 优先根据 pid 文件停止。
- 对每个 pid，先做 ownership 校验：
  - `ps -p <pid> -o args=`
  - 必须同时匹配：
    - `BIN_DIR/<app_target>`
    - `--config <sibling cluster.json>`
    - `--node_id <expected node_id>`
- 只有通过上述校验，才会发送信号。
- 停止策略：
  - 先 `TERM`
  - 等待退出
  - 仍未退出时再 `KILL`
- stale pid / 缺失 pid：
  - 缺失 pid：输出 `skip ... reason=missing_pid_file`
  - pid 非数字：删除 stale pid 文件并诊断
  - pid 已不存在：删除 stale pid 文件并诊断
  - pid 存在但不属于当前 009 example：删除 stale pid 文件并诊断，不 kill

## Boundary Preservation

- 008 baseline：保留
  - 未修改 `examples/object-storage-local-3meta-6store/tingzhi.sh`
- 不依赖全局同名进程搜索，不做 `pkill` / `killall`。
- 缺失 pid 文件时不会误杀任何全局同名进程。
- log 文件保留，不删除。
- `data_dir` / `snapshot_dir` / `identity` / raft data / chunk data 均保留，不删除。
- 未把 `store-7`、`meta-4`、`meta-5` 当成已启动节点强制停止。

## Validation

- build 命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client ) 9>/tmp/cqupt_raft_build.lock`
- build 结果：`PASS`
- build 日志：
  - `tmp/test-logs/t091-build.log`

## Startup + Shutdown Smoke

- 启动脚本：
  - `examples/object-storage-local-009-dynamic/qidong.sh`
- 停机脚本：
  - `examples/object-storage-local-009-dynamic/tingzhi.sh`
- smoke 在同一 shell 会话中执行，避免当前执行环境在父命令结束后回收后台子进程造成误判。
- 关键日志：
  - `tmp/test-logs/t091-startup.log`
  - `tmp/test-logs/t091-startup-pids.log`
  - `tmp/test-logs/t091-status.log`
  - `tmp/test-logs/t091-shutdown.log`
  - `tmp/test-logs/t091-shutdown-second.log`

## Smoke Result

- startup 后 pid 文件存在：`PASS`
  - `view-1/view-2/meta-1..3/store-1..6` 的 pid 文件全部生成
- startup 后进程存活：`PASS`
  - `tmp/test-logs/t091-status.log` 记录全部节点为 `RUNNING`
- shutdown 后 pid 对应进程全部退出：`PASS`
  - `tmp/test-logs/t091-status.log` 记录全部节点为 `STOPPED`
- shutdown 顺序：`PASS`
  - `tmp/test-logs/t091-shutdown.log` 记录顺序为：
    - `store-6 -> store-1`
    - `meta-3 -> meta-1`
    - `view-2 -> view-1`
- 再执行一次 shutdown：`PASS`
  - `tmp/test-logs/t091-shutdown-second.log` 全部输出 `skip ... reason=missing_pid_file`
  - 说明脚本可幂等执行，不会把“未启动节点”视为严重失败
- log 文件保留：`PASS`
  - `examples/object-storage-local-009-dynamic/logs/view-1.log`
  - `examples/object-storage-local-009-dynamic/logs/view-2.log`
  - `examples/object-storage-local-009-dynamic/logs/meta-1.log`
  - `examples/object-storage-local-009-dynamic/logs/store-1.log`
  等文件仍在
- `data_dir` / identity 保留：`PASS`
  - `nodes/view-1/data/node.identity`
  - `nodes/meta-1/data/node.identity`
  - `nodes/store-1/data/node.identity`
  等文件仍在

## Stale PID Handling Check

- 额外验证：
  - 手工创建 `examples/object-storage-local-009-dynamic/pids/store-1.pid`，内容为不存在的 pid `999999`
  - 再执行 `examples/object-storage-local-009-dynamic/tingzhi.sh`
- 结果：`PASS`
  - 日志：`tmp/test-logs/t091-stale-pid.log`
  - 观察到：
    - `remove stale pid file node_id=store-1 pid=999999 reason=process_not_running`
  - 说明 stale pid 会被诊断并清理，不会误杀其他进程

## How Unrelated Processes Are Protected

- shutdown 不会根据节点名全局搜进程。
- 只有 pid 文件指向的进程同时满足以下条件才会被 kill：
  - 二进制路径匹配当前 repo 的 `BIN_DIR/<app_target>`
  - `--config` 指向当前 sibling `cluster.json`
  - `--node_id` 匹配当前目标节点
- 若 pid 被系统复用给无关进程：
  - 脚本只删除 stale pid 文件
  - 不会发送 `TERM` / `KILL`

## Result

- 最终状态：`PASS`
- 是否已勾选 `T091`：是
- 是否可以进入 `T092`：可以
