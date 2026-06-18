# T092 Add Run-Time StorageNode Join Command

## Scope

- 任务类型：example 脚本 / 验证 / 文档
- 本任务为 sibling 009 local RPC example 增加运行中 `StorageNode` join 命令。
- 本任务不实现 Metadata learner join，不实现 ViewNode failover roundtrip，不修改生产代码。

## Task Source

- `tasks.md`: `T092`
- `plan.md`: Phase 10 local RPC example / dynamic join
- `contracts/storage-dynamic-join.md`
- `contracts/local-rpc-validation.md`
- 前置报告：
  - `t089-add-or-extend-009-local-rpc-topology-config.md`
  - `t090-add-2-viewnode-startup-support.md`
  - `t091-add-matching-shutdown-support.md`

## Files Changed

- `examples/object-storage-local-009-dynamic/rpc_demo.sh`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t092-add-runtime-storagenode-join-command.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- 未修改生产代码
- 未修改测试代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 在 sibling 009 script `examples/object-storage-local-009-dynamic/rpc_demo.sh` 新增并收口 `join-storage` 运行时命令。
- `join-storage` 只在 T090 startup 之后启动 `store-7`，不会把它放入初始静态启动列表。
- 新增运行时 `store-7` 的独立路径：
  - config: `examples/object-storage-local-009-dynamic/storage-join-store-7.json`
  - pid: `examples/object-storage-local-009-dynamic/pids/store-7.pid`
  - log: `examples/object-storage-local-009-dynamic/logs/store-7.log`
  - data: `examples/object-storage-local-009-dynamic/nodes/store-7/data`
  - identity: `examples/object-storage-local-009-dynamic/nodes/store-7/data/node.identity`
- `join-storage` 会启动 `storage_node_app`，等待 `status` 中观察到 `store-7` 为 `liveness=live`，并确认本地 identity 已可复用或已创建。
- 为了让 `roundtrip` 真正验证“运行中加入后后续新对象写入仍可用”，补了 bucket 预检查/自动创建：
  - 先从 `storage_client status` 解析 metadata leader endpoint
  - 再用 `raft_metadata_client list-objects/create-bucket` 确保 bucket 存在
- `roundtrip` 结束后输出 best-effort placement 观察，不把“未直接观察到新节点承载 payload”误写成失败。

## Boundary Checks

- 没有修改生产代码
- 没有修改测试断言
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 没有把 ViewNode 当成 Raft membership authority
- 没有让 StorageNode join 进入 Raft log
- 保持 committed membership authority 仍由 Raft 决定
- 没有修改 008 baseline `examples/object-storage-local-3meta-6store/*`

## Validation

- 构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client ) 9>/tmp/cqupt_raft_build.lock
```

- 构建结果：`PASS`
- 构建日志：`tmp/test-logs/t092-build.log`
- 构建摘要：`ninja: no work to do.`

- 脚本命令：

```bash
examples/object-storage-local-009-dynamic/qidong.sh
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh join-storage
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip
examples/object-storage-local-009-dynamic/tingzhi.sh
```

- 脚本结果：`PASS`
- 关键日志：
  - startup: `tmp/test-logs/t092-startup.log`
  - status before join: `tmp/test-logs/t092-status-before.log`
  - join-storage: `tmp/test-logs/t092-join.log`
  - status after join: `tmp/test-logs/t092-status-after.log`
  - roundtrip: `tmp/test-logs/t092-roundtrip.log`
  - cleanup: `tmp/test-logs/t092-cleanup.log`
  - validation summary: `tmp/test-logs/t092-status-summary.log`
  - no-residual-process check: `tmp/test-logs/t092-live-pids.log`
  - placement observation: `tmp/test-logs/t092-placement.log`

## Validation Notes

- pre-join 不是静态启动 `store-7`
  - `t092-startup.log` 只记录 `store-1..store-6`
  - join 前不存在 `pids/store-7.pid`
  - `t092-status-summary.log` 记录：`before_join:startup_has_only_store_1_to_store_6:no_store7_pid`
- 运行中 join 成功
  - `t092-join.log` 记录 `started node_id=store-7`
  - 同一日志内记录 `observed node_id=store-7 in cluster status as LIVE`
  - `t092-status-after.log` 可见 `storage_node node_id=store-7 ... liveness=live`
- identity / log / pid / data 路径验证
  - `pids/store-7.pid` 在 join 后生成
  - `logs/store-7.log` 存在
  - `nodes/store-7/data/node.identity` 存在
  - 最新 Linux 验证中 `store-7` 复用了已有 identity：
    - `identity_loaded_existing=true`
    - `identity_created_new=false`
- roundtrip 成功
  - `t092-roundtrip.log` 中 4 个文件均完成 upload/download
  - `verify` 结果为：
    - `server.jar`: `OK`
    - `test_file.deb`: `OK`
    - `test_file.zip`: `OK`
    - `区域扩散.pdf`: `OK`
- placement 观察
  - `t092-placement.log` 记录：
    - `dynamic storage participation not directly observed node_id=store-7 payload_files_before=0 payload_files_after=0`
  - 该结果不构成失败，因为 T092 只要求“优先验证” placement；本次已证明 `store-7` 出现在 discovery/status，并且 join 后新对象写入与下载路径可用。

## Cleanup

- 本任务实际启动并运行了以下进程：
  - `view-1`
  - `view-2`
  - `meta-1`
  - `meta-2`
  - `meta-3`
  - `store-1..store-6`
  - 动态 `store-7`
- cleanup 执行方式：
  - 先调用 `examples/object-storage-local-009-dynamic/tingzhi.sh`
  - 再按 pid + ownership 校验手动停止动态 `store-7`
- `t092-cleanup.log` 记录：
  - `store-6 -> store-1`
  - `meta-3 -> meta-1`
  - `view-2 -> view-1`
  - `stopped dynamic store-7 pid=...`
- `t092-live-pids.log` 结果：
  - `view-1..view-2`: `DEAD`
  - `meta-1..meta-3`: `DEAD`
  - `store-1..store-7`: `DEAD`
- 结论：无本 example 残留进程

## Build Lock

- 使用了 `flock` 构建锁
- 已获得锁
- 构建未被跳过

## Platform Notes

- Linux：已验证，结果 `PASS`
- Windows：`pending`
- macOS：`pending`

## Risks / Follow-ups

- `status` 在 startup 刚结束时可能短时间尚未收敛到全部初始 StorageNode，可见性会晚于 pid/log 落盘；因此本任务对“不是静态启动 `store-7`”的判定以 startup log、缺失 `store-7.pid`、join 后 `store-7` 变为 `live` 为准。
- 动态 `store-7` 在本次 roundtrip 中未直接观察到 payload 文件增长，说明 placement 参与仍只做到 best-effort 观测；如后续 T095/T098 需要更强展示，应在 example 层补更直接的 placement 观测命令或输出。
- 本任务未实现 Metadata learner join，后续进入 `T093`。

## Result

- 最终状态：`PASS`
- 是否已勾选 `T092`：是
- 是否可以进入下一任务：可以，进入 `T093`
