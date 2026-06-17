# T098 Record Local RPC Dynamic Validation Results

## Scope

- 任务类型：local RPC example 动态验证 / 文档
- 使用 example：
  - `examples/object-storage-local-009-dynamic/`
- 本任务不修改生产代码、不修改测试、不修改 example 逻辑。

## T089-T097 Status Summary

- `T089`: PASS，009 sibling topology config 已就位
- `T090`: PASS，2 ViewNode startup 已就位
- `T091`: PASS，matching shutdown 已就位
- `T092`: PASS，runtime StorageNode join 命令已就位
- `T093`: PASS，单 learner join + blocked promote 观测命令已就位
- `T094`: PASS，第二 learner join + batch promote 观测命令已就位
- `T095`: PASS，ViewNode failover 脚本路径已就位
- `T096`: PASS，日志 / pid / tmp 路径已落到本地 ignored 路径
- `T097`: PASS，app targets targeted build 已完成
  - 引用报告：
    - `specs/009-local-rpc-object-storage-stabilization/task-reports/t097-run-targeted-app-build-before-local-rpc-validation.md`

## Validation Command Sequence

本次 Linux 实测按 009 sibling example 顺序执行：

```bash
examples/object-storage-local-009-dynamic/qidong.sh
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip
examples/object-storage-local-009-dynamic/rpc_demo.sh join-storage
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip
examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh promote-metadata-learner || true
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh join-metadata-learner-2
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh promote-metadata-learners || true
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view
```

脚本在 `failover-view` 返回非零后停止，因此：

- `examples/object-storage-local-009-dynamic/rpc_demo.sh status`（failover 之后的独立一步）未执行
- `examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip`（failover 之后的独立一步）未执行

## Linux Validation Result

### 1. Startup

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-startup.log`
  - 启动摘要：
    - `startup complete: views=2 metadata=3 storage=6`

### 2. Initial Status

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-status-01.log`
- 关键观察：
  - `target_endpoint=127.0.0.1:8301`
  - `view_nodes=2`
  - `metadata_nodes=3`
  - `storage_nodes=6`
  - `leader_hint` 可见
  - `non_authority_boundary ... raft_membership_authority=false object_manifest_authority=false`

### 3. Initial Roundtrip

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-roundtrip-01.log`
- 关键观察：
  - 多个 `download OK ... integrity=PASS`
  - 4 条 `[verify] OK`
  - 真实覆盖 `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp`

### 4. Runtime StorageNode Join

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-join-storage.log`
  - `tmp/test-logs/t098-status-02.log`
- 关键观察：
  - `join-storage` 启动了 `store-7`
  - `status` 变为 `storage_nodes=7`
  - 可见：
    - `storage_node node_id=store-7 ... liveness=live`
  - `join-storage` 输出：
    - `observed node_id=store-7 in cluster status as LIVE`

### 5. Roundtrip After Storage Join

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-roundtrip-02.log`
- 关键观察：
  - 多个 `download OK ... integrity=PASS`
  - 4 条 `[verify] OK`
  - `store-7` 已进入 storage snapshot 观测
  - 但脚本仍记录：
    - `[placement] dynamic storage participation not directly observed node_id=store-7 ...`
- 结论：
  - join 后后续写入/读取路径仍可用
  - 但本次日志没有直接证明新对象 placement 一定实际落到了 `store-7`

### 6. Metadata Learner 1 Join

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-join-metadata-learner.log`
  - `tmp/test-logs/t098-status-03.log`
  - `tmp/test-logs/t098-status-04.log`
- 关键观察：
  - `meta-4` 运行中启动，不在初始 voter 列表
  - `status` 可见：
    - `metadata_node node_id=meta-4 ... raft_role=learner membership_observation=learner`
  - learner 诊断可见：
    - `committed_voter_count=3`
    - `committed_quorum_size=2`
    - `learner_status=ready_to_promote`
    - `promotion_status=waiting_for_pair`
    - `promotion_block_reason=even_voter_count`

### 7. Single Learner Promote Observation

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-promote-metadata-learner.log`
- 关键观察：
  - 输出：
    - `[promote-metadata-learner] blocked promote observed node_id=meta-4`
  - 仍保持：
    - `committed_voter_count=3`
    - `committed_quorum_size=2`
  - 未形成 committed 4-voter membership

### 8. Metadata Learner 2 Join

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-join-metadata-learner-2.log`
  - `tmp/test-logs/t098-status-05.log`
- 关键观察：
  - `meta-5` 运行中启动，不在初始 voter 列表
  - `status` 可见：
    - `metadata_node node_id=meta-5 ... raft_role=learner membership_observation=learner`
  - promote 前仍是：
    - `committed_voter_count=3`
    - `committed_quorum_size=2`

### 9. Batch Promote Observation

- 结果：`PASS`
- 证据：
  - `tmp/test-logs/t098-promote-metadata-learners.log`
  - `tmp/test-logs/t098-status-06.log`
  - `tmp/test-logs/t098-failover-view.log`
- promote 前观测：
  - `observed two non-voter learners while committed voters remain 3 and quorum remains 2`
- promote 后观测：
  - `batch promote observed via cluster status`
  - `committed_voter_count=5`
  - `committed_quorum_size=3`
  - `committed_voter_ids=[1,2,3,4,5]`
  - `promotion_status=batch_promoted`
  - `promotion_batch_size=2`
- 本次运行中未观察到 committed `4-voter` state。

### 10. ViewNode Failover

- 结果：`FAIL`
- 证据：
  - `tmp/test-logs/t098-failover-view.log`
- 关键失败：
  - `FAIL reason=surviving_view_status_unavailable node_id=view-2 endpoint=127.0.0.1:8302 wait_seconds=30`
- 已确认行为：
  - `view-1` 被停止
  - `view-2` 继续运行
  - `failover-view` 日志内部包含 `target_endpoint=127.0.0.1:8302` 的 status 输出
  - `view-2.log` 同时显示 peer sync 在 `view-1` 停止后持续收到 connection refused/backoff
- 结论：
  - 当前 full dynamic sequence 下，`failover-view` 命令未稳定通过
  - 因该命令失败，failover 之后的独立 `status` / `roundtrip` 步骤未执行，不能记成 PASS

## Committed Voters / Quorum Summary

- 单 learner blocked promote 阶段：
  - `committed_voter_count=3`
  - `committed_quorum_size=2`
- 两 learner batch promote 之后：
  - `committed_voter_count=5`
  - `committed_quorum_size=3`
- 本次运行中未观察到 committed `4-voter` state

## Log Paths

- startup:
  - `tmp/test-logs/t098-startup.log`
- status:
  - `tmp/test-logs/t098-status-01.log`
  - `tmp/test-logs/t098-status-02.log`
  - `tmp/test-logs/t098-status-03.log`
  - `tmp/test-logs/t098-status-04.log`
  - `tmp/test-logs/t098-status-05.log`
  - `tmp/test-logs/t098-status-06.log`
- roundtrip:
  - `tmp/test-logs/t098-roundtrip-01.log`
  - `tmp/test-logs/t098-roundtrip-02.log`
- dynamic join / promote / failover:
  - `tmp/test-logs/t098-join-storage.log`
  - `tmp/test-logs/t098-join-metadata-learner.log`
  - `tmp/test-logs/t098-join-metadata-learner-2.log`
  - `tmp/test-logs/t098-promote-metadata-learner.log`
  - `tmp/test-logs/t098-promote-metadata-learners.log`
  - `tmp/test-logs/t098-failover-view.log`
- cleanup:
  - `tmp/test-logs/t098-cleanup.log`

报告只记录摘要、关键状态和日志路径，不粘贴完整大日志。

## Cleanup

- 已执行 cleanup：是
- cleanup 方式：
  - 先调用 `examples/object-storage-local-009-dynamic/tingzhi.sh`
  - 再按 pid-file ownership 兜底停止动态节点：
    - `store-7`
    - `meta-4`
    - `meta-5`
- cleanup 结果：
  - `tingzhi.sh` 停掉了初始 `view-1/view-2/meta-1..3/store-1..6`
  - pid-file fallback 额外停掉动态节点
  - 任务结束后 `pgrep -af '/home/yangjilei/Code/C\\+\\+/CQUPT_Raft/build/linux/(view_node_app|metadata_node_app|storage_node_app)'` 无输出

## Platform Notes

- Linux：已实测，结果见本报告
- Windows：`pending`
- macOS：`pending`

## Result

- 最终状态：`FAIL`
- 失败原因：
  - `failover-view` 在 full dynamic sequence 末尾返回 `surviving_view_status_unavailable`
  - 因此无法把 “failover 后 surviving ViewNode status / roundtrip” 记成 PASS
- 是否已勾选 `T098`：否
- 是否可以进入下一任务：不建议；应先修复 Phase 10 failover runtime 失败，再进入 `T099`
