# T094 Add Second Learner Join And Batch Promote Observation

## Scope

- 任务类型：example 脚本 / 验证 / 文档
- 本任务扩展 sibling `009` local RPC example，补齐第二个 Metadata learner 的运行中加入与 batch promote 结果观测。
- 本任务不实现 ViewNode failover roundtrip，不实现新的生产协议。

## Task Source

- `tasks.md`: `T094`
- `plan.md`: Phase 10 local RPC example / two learners / batch promote observation
- 前置任务：
  - `T089`: 009 topology config 已完成
  - `T090`: 2-ViewNode startup 已完成
  - `T091`: matching shutdown 已完成
  - `T093`: 单 learner join + blocked promote 已完成
  - `T088`: Phase 9 batch promote Linux targeted validation 已记录为 `PASS`

## Files Changed

- `examples/object-storage-local-009-dynamic/rpc_demo.sh`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t094-add-second-learner-join-and-batch-promote-observation.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Production Code Note

- 本任务未新增生产代码。
- 但本次 example 观测依赖当前工作树中已存在的 `apps/metadata_node_app.cpp` 修复：
  - promote 后动态 candidate 不再一直上报为 learner
  - candidate join 线程在本地 committed role 变为 voter 后退出
- 如果回退该修复，`meta-4`/`meta-5` 在 example 里的 promote 后 voter 观测会退化。

## What Changed

### 1. `rpc_demo.sh` 增加第二 learner 运行时入口

- 新增 `join-metadata-learner-2`
- 两个动态 learner 路径固定且相互独立：
  - `meta-4`
    - config: `examples/object-storage-local-009-dynamic/metadata-learner-4.json`
    - identity: `examples/object-storage-local-009-dynamic/nodes/meta-4/data/node.identity`
    - data: `examples/object-storage-local-009-dynamic/nodes/meta-4/data`
    - snapshot: `examples/object-storage-local-009-dynamic/nodes/meta-4/snapshots`
    - pid: `examples/object-storage-local-009-dynamic/pids/meta-4.pid`
    - log: `examples/object-storage-local-009-dynamic/logs/meta-4.log`
  - `meta-5`
    - config: `examples/object-storage-local-009-dynamic/metadata-learner-5.json`
    - identity: `examples/object-storage-local-009-dynamic/nodes/meta-5/data/node.identity`
    - data: `examples/object-storage-local-009-dynamic/nodes/meta-5/data`
    - snapshot: `examples/object-storage-local-009-dynamic/nodes/meta-5/snapshots`
    - pid: `examples/object-storage-local-009-dynamic/pids/meta-5.pid`
    - log: `examples/object-storage-local-009-dynamic/logs/meta-5.log`

### 2. 每次 join 前重置停机 learner 运行态

- 对 `meta-4`/`meta-5`：
  - 如果进程未运行，则删除旧 `pid/log/data/snapshot`
  - 避免复用上一次验证遗留的 promoted voter 身份
- 这一步只作用于 sibling `009` example 的本地 runtime 目录，不影响 `008` baseline。

### 3. `promote-metadata-learners` 改为“真实观察”而非伪触发

- 先验证 promote 前条件：
  - `meta-4`、`meta-5` 都是运行中加入的 learner/non-voter
  - learner 日志仍为 `committed_voter_count=3`
  - learner 日志仍为 `committed_quorum_size=2`
- 然后轮询 `status`，直到：
  - `metadata_nodes=5`
  - `meta-4` `membership_observation=voter`
  - `meta-5` `membership_observation=voter`
- 说明：
  - 运行中的 learner promote 诊断行存在 stdout flush 时机差异
  - 因此在线命令以 cluster `status` 为成功信号
  - `quorum=3` 与 “无 committed 4-voter history” 由关停后的 learner 日志落盘证据补齐

## Boundary Checks

- 未修改 `008` baseline example
- 未把 `meta-4`/`meta-5` 放进初始 voters
- `meta-4`/`meta-5` 本地配置仍是 `initial_role=candidate`
- ViewNode 只用于 discovery / observation，不是 membership authority
- 未弱化测试
- 未新增生产协议

## Validation

- 构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client ) 9>/tmp/cqupt_raft_build.lock
```

- 构建结果：`PASS`

- 运行命令：

```bash
examples/object-storage-local-009-dynamic/qidong.sh
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner
examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner-2
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh promote-metadata-learners
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/tingzhi.sh
```

- Linux 验证结果：`PASS`
- 验证日志：
  - startup: `tmp/test-logs/t094-startup.log`
  - status before join: `tmp/test-logs/t094-status-before.log`
  - join learner 1: `tmp/test-logs/t094-join-learner-1.log`
  - join learner 2: `tmp/test-logs/t094-join-learner-2.log`
  - status after join: `tmp/test-logs/t094-status-after-join.log`
  - promote observe: `tmp/test-logs/t094-promote.log`
  - status after promote: `tmp/test-logs/t094-status-after-promote.log`
  - cleanup: `tmp/test-logs/t094-cleanup.log`
  - summary: `tmp/test-logs/t094-summary.log`

## Evidence

### 1. 两个 learners 是运行中加入，不是静态 voters

- `t094-status-before.log`
  - `metadata_nodes=3`
- `t094-join-learner-1.log`
  - 启动 `meta-4`
  - `status` 观测 `metadata_nodes=4`
  - `meta-4 ... membership_observation=learner`
- `t094-join-learner-2.log`
  - 启动 `meta-5`
  - `status` 观测 `metadata_nodes=5`
  - `meta-5 ... membership_observation=learner`

### 2. promote 前 committed voters 仍为 3，quorum 仍为 2

- `tmp/test-logs/t094-summary.log`
  - `meta4_pre=... committed_voter_count=3 ... committed_quorum_size=2 ... learner_status=pending`
  - `meta5_pre=... committed_voter_count=3 ... committed_quorum_size=2 ... learner_status=pending`
- `tmp/test-logs/t094-promote.log`
  - `observed two non-voter learners while committed voters remain 3 and quorum remains 2`

### 3. batch promote 后 committed voters 变为 5，quorum 变为 3

- 在线 `status` 证据：
  - `tmp/test-logs/t094-status-after-promote.log`
    - `status OK ... metadata_nodes=5`
    - `meta-4 ... membership_observation=voter`
    - `meta-5 ... membership_observation=voter`
- learner 落盘日志证据：
  - `examples/object-storage-local-009-dynamic/logs/meta-4.log:13`
    - `committed_voter_count=5`
    - `committed_quorum_size=3`
    - `promotion_status=batch_promoted`
  - `examples/object-storage-local-009-dynamic/logs/meta-5.log:10`
    - `candidate join settled`
    - `committed_voter_count=5`
    - `committed_quorum_size=3`

### 4. 没有 committed 4-voter membership

- `tmp/test-logs/t094-summary.log`
  - `committed_four_history=absent`
- 本轮 learner 日志检索未出现：
  - `committed_voter_count=4`

### 5. 在线 promote 观察命令的边界

- `t094-promote.log` 最终记录：
  - `batch promote observed via cluster status; learner promote log may flush after shutdown`
- 这不是伪造成功：
  - 在线路径确实看到 `meta-4`、`meta-5` 变为 voter
  - 离线路径再用 learner 落盘日志补齐 `quorum=3` 与 `batch_promoted` 证据

## Cleanup

- cleanup 方式：
  - 先调用 `examples/object-storage-local-009-dynamic/tingzhi.sh`
  - 再按 pid + ownership 单独停止 `meta-4` / `meta-5`
  - `store-7` 本次未启动
- `t094-cleanup.log` 记录：
  - `store-6 -> store-1` 已停止
  - `meta-3 -> meta-1` 已停止
  - `view-2 -> view-1` 已停止
  - `meta-4` / `meta-5` 已停止
- residual check：
  - 未发现本 example 残留进程

## Platform Notes

- Linux：`PASS`
- Windows：`pending`
- macOS：`pending`

## Result

- 最终状态：`PASS`
- 是否已勾选 `T094`：是
- 是否可以进入下一任务：可以，进入 `T095`
