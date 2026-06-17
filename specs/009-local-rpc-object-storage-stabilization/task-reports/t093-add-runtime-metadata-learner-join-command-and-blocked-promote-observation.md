# T093 Add Run-Time Metadata Learner Join Command And Blocked Promote Observation

## Scope

- 任务类型：example 脚本 / 最小入口修复 / 验证 / 文档
- 本任务为 sibling 009 local RPC example 增加运行中 `MetadataNode` learner join 命令与 blocked single promote 观测命令。
- 本任务不实现第二个 learner join，不实现 batch promote，不实现 ViewNode failover roundtrip。

## Task Source

- `tasks.md`: `T093`
- `plan.md`: Phase 10 local RPC example / runtime metadata learner join
- 前置任务：
  - `T089`: 009 topology config 已完成
  - `T090`: 2-ViewNode startup 已完成
  - `T091`: matching shutdown 已完成
  - `T092`: runtime StorageNode join 已完成
  - `T088`: Phase 9 batch promote Linux targeted validation 已记录为 `PASS`

## Files Changed

- `examples/object-storage-local-009-dynamic/rpc_demo.sh`
- `apps/metadata_node_app.cpp`
- `modules/raft/node/raft_node.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t093-add-runtime-metadata-learner-join-command-and-blocked-promote-observation.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Why Minimal Production Fix Was Required

- 仅改 example 脚本无法完成本任务。
- 原因 1：
  - `metadata_node_app` 的 `initial_role=candidate` 模式原先只做一次 `JoinMetadataCluster` admission 校验，然后直接退出。
  - 这会导致 `meta-4` 无法在运行中的 example 里继续启动、接收 catch-up、向 ViewNode heartbeat，也无法被 `status` 观察为 learner。
- 原因 2：
  - 即使让 candidate 模式继续启动，本地 `RaftNode` 的 committed-membership 诊断原先仍把 `config_.node_id` 计入 committed voter set。
  - 结果会把 `meta-4` 的本地启动日志错误写成 `committed_voter_count=4, quorum=3`，与 “单 learner 不能本地变成 voter” 的任务目标冲突。

## What Changed

### 1. `rpc_demo.sh` 新增运行时 Metadata learner 命令

- 新增 `join-metadata-learner`
  - 只在 T090 startup 之后启动动态 `meta-4`
  - 不把 `meta-4` 放进初始静态 voters 列表
  - 路径边界：
    - config: `examples/object-storage-local-009-dynamic/metadata-learner-4.json`
    - pid: `examples/object-storage-local-009-dynamic/pids/meta-4.pid`
    - log: `examples/object-storage-local-009-dynamic/logs/meta-4.log`
    - data: `examples/object-storage-local-009-dynamic/nodes/meta-4/data`
    - identity: `examples/object-storage-local-009-dynamic/nodes/meta-4/data/node.identity`
    - snapshot: `examples/object-storage-local-009-dynamic/nodes/meta-4/snapshots`
- 新增 `promote-metadata-learner`
  - 该命令不是伪造 promote 成功
  - 它只等待并输出真实 blocked promote 证据：
    - `learner_status=ready_to_promote`
    - `promotion_status=waiting_for_pair`
    - `promotion_block_reason=even_voter_count`
    - `committed_voter_count=3`
    - `committed_quorum_size=2`
- `status` 追加 learner diagnostics 摘要，但只在 `meta-4.pid` 存在且进程归属当前 009 example 时输出，避免 stale log 混入当前状态。
- 失败清理：
  - `join-metadata-learner` 失败时只按 pid + ownership 回收 `meta-4`
  - 不误杀其他窗口或其他 example 的同名进程

### 2. `metadata_node_app.cpp` 最小入口修复

- candidate 模式在 bootstrap `JoinMetadataCluster` admission 成功后不再直接退出。
- 进程继续启动本地 `RaftNode`，并周期性重放 `JoinMetadataCluster` 观测：
  - `learner_status=pending`
  - `learner_status=ready_to_promote`
  - `promotion_status=waiting_for_pair`
- 对 ViewNode 的 registration / heartbeat 观测在 dynamic candidate 已通过 join admission 后明确上报为 learner：
  - `raft_role=learner`
  - `membership_observation=learner`
- 这仍然只是 discovery / observation 输入，不把 ViewNode 变成 membership authority。

### 3. `raft_node.cpp` 本地 committed-membership 诊断修复

- 对本地 `RuntimeMembershipRole::kLearner / kNonMember` 节点：
  - committed-membership 诊断不再把自身计入 committed voter set
  - startup log 也不再把自身计入 `committed_voter_count` / `quorum`
- 该修复只收紧本地非 voter 节点的诊断边界，不改变现有 3-voter leader 的 committed membership authority。

## Boundary Checks

- 未修改测试断言
- 未修改 proto
- 未修改 008 baseline `examples/object-storage-local-3meta-6store/*`
- 未把 `meta-4` 本地配置成 voter
- 未把 ViewNode 变成 Metadata/Raft membership authority
- 未声称 batch promote 已完成
- 未实现第二 learner join
- 未实现 5-voter promote

## Validation

- 构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client ) 9>/tmp/cqupt_raft_build.lock
```

- 构建结果：`PASS`
- 构建日志：`tmp/test-logs/t093-build.log`

- 运行命令：

```bash
examples/object-storage-local-009-dynamic/qidong.sh
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh promote-metadata-learner
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/tingzhi.sh
```

- Linux 验证结果：`PASS`
- 验证日志：
  - pre-clean: `tmp/test-logs/t093-preclean.log`
  - startup: `tmp/test-logs/t093-startup.log`
  - status before join: `tmp/test-logs/t093-status-before.log`
  - join command: `tmp/test-logs/t093-join.log`
  - status after join: `tmp/test-logs/t093-status-after-join.log`
  - blocked promote observation: `tmp/test-logs/t093-promote-blocked.log`
  - status after promote observation: `tmp/test-logs/t093-status-after-promote.log`
  - cleanup: `tmp/test-logs/t093-cleanup.log`
  - residual check: `tmp/test-logs/t093-live-pids.log`
  - summary: `tmp/test-logs/t093-summary.log`

## Evidence

### 1. `meta-4` 是运行中加入，不是静态 voter

- `t093-status-before.log` 记录：
  - `metadata_nodes=3`
- `t093-join.log` 记录：
  - `started node_id=meta-4`
  - join 前不是静态启动列表成员

### 2. join 通过 Metadata leader committed membership path 进入 learner

- `t093-join.log` / `logs/meta-4.log` 记录 bootstrap accepted：
  - `disposition=JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT`
  - `requested_membership=JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER`
  - `join_authority=metadata_leader_committed_membership_only`
  - `requested_membership=learner_not_voter`

### 3. learner 可被 status / diagnostics 观察

- `t093-status-after-join.log`：
  - `metadata_nodes=4`
  - `metadata_node node_id=meta-4 ... raft_role=learner membership_observation=learner`
- `t093-join.log` / `logs/meta-4.log`：
  - `learner_status=pending`
  - `promotion_status=catching_up`

### 4. 单 learner promote 被 blocked

- `t093-promote-blocked.log`：
  - `learner_status=ready_to_promote`
  - `promotion_status=waiting_for_pair`
  - `promotion_block_reason=even_voter_count`
- `t093-status-after-promote.log` 仍显示：
  - `raft_role=learner`
  - `membership_observation=learner`

### 5. committed voters 仍为 3，quorum 仍为 2

- `logs/meta-4.log` startup 行已变为：
  - `committed_voter_count=3, quorum=2`
- join / blocked promote 诊断持续显示：
  - `committed_voter_count=3`
  - `committed_quorum_size=2`
  - `committed_voter_ids=[1,2,3]`

### 6. 没有形成 committed 4-voter membership

- 本次新日志未出现：
  - `committed_voter_count=4`
  - `batch_promoted`
  - `committed_membership_changed=true`
- blocked promote 观测仍明确停留在：
  - `promotion_status=waiting_for_pair`
  - `promotion_block_reason=even_voter_count`

## Cleanup

- cleanup 执行方式：
  - 先调用 `examples/object-storage-local-009-dynamic/tingzhi.sh`
  - 再按 pid + ownership 单独停止 `meta-4`
  - `store-7` 本次未启动，如存在则同样按 pid + ownership 清理
- `t093-cleanup.log` 记录：
  - `store-6 -> store-1`
  - `meta-3 -> meta-1`
  - `view-2 -> view-1`
  - `stopped node_id=meta-4`
- `t093-live-pids.log` 结果：
  - `view-1..view-2`: `DEAD`
  - `meta-1..meta-4`: `DEAD`
  - `store-1..store-7`: `DEAD`
- 结论：无本 example 残留进程

## Platform Notes

- Linux：`PASS`
- Windows：`pending`
- macOS：`pending`

## Result

- 最终状态：`PASS`
- 是否已勾选 `T093`：是
- 是否可以进入下一任务：可以，进入 `T094`
