# T095 Add ViewNode Failover Status And Roundtrip Path

## Scope

- 任务类型：example 脚本 / failover 验证 / 文档
- 本任务在 sibling `009` local RPC example 中增加 ViewNode failover 验证路径。
- 本任务不修改生产代码，不实现新的 ViewNode peer sync 逻辑。

## Task Source

- `tasks.md`: `T095`
- `plan.md`: Phase 10 local RPC example / ViewNode failover status + roundtrip
- 前置任务：
  - `T089`: 009 topology config 已完成
  - `T090`: 2-ViewNode startup 已完成
  - `T091`: matching shutdown 已完成
  - `T092-T094`: runtime join / learner / batch promote example 能力已可用，但不是本任务前提结论

## Files Changed

- `examples/object-storage-local-009-dynamic/rpc_demo.sh`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t095-add-viewnode-failover-status-and-roundtrip-path.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### 1. `rpc_demo.sh` 新增 `failover-view`

- 新命令：
  - `examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view`
- 行为：
  - 只停止 `view-1`
  - 不停止任何 MetadataNode / StorageNode
  - 停止后轮询 surviving `view-2`
  - 等到 `status` 在 `127.0.0.1:8302` 返回可用集群视图才算成功

### 2. failover 后自动切 survivor-only client config

- 当前 `storage_client status` 只使用配置里的单个 `view_endpoint`，不会自动对第二个 ViewNode 做 status RPC fallback。
- 为了满足本任务“真实通过 surviving ViewNode 完成 status / roundtrip”的要求，example 脚本在 `view-1` 已停且 `view-2` 仍存活时：
  - 动态生成 `logs/failover-view-2-client.json`
  - 注入显式 `view_endpoint=127.0.0.1:8302`
  - `status` / `upload` / `download` / `roundtrip` 自动切到这个 survivor-only config
- 这只是 example 脚本输入切换，不是生产代码改动。

### 3. roundtrip 增加 surviving ViewNode 断言

- `roundtrip` 开始前先做一次 `status`
- 如果发现 `view-1` 已停、`view-2` 存活：
  - 必须确认 `target_endpoint=127.0.0.1:8302`
  - 必须确认 Metadata 和 Storage live 观测仍然可用
  - 然后才继续真实 `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp`

## Boundary Checks

- 未修改 `008` baseline example
- 未停止整个集群
- 未把 ViewNode 变成 Raft membership authority
- 未修改生产代码
- failover 后 Metadata quorum 仍由 Metadata/Raft 保持，不依赖 ViewNode

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
examples/object-storage-local-009-dynamic/rpc_demo.sh failover-view
examples/object-storage-local-009-dynamic/rpc_demo.sh status
examples/object-storage-local-009-dynamic/rpc_demo.sh roundtrip
examples/object-storage-local-009-dynamic/tingzhi.sh
```

- Linux 验证结果：`PASS`
- 验证日志：
  - startup: `tmp/test-logs/t095-startup.log`
  - status before failover: `tmp/test-logs/t095-status-before.log`
  - failover: `tmp/test-logs/t095-failover.log`
  - status after failover: `tmp/test-logs/t095-status-after-failover.log`
  - roundtrip: `tmp/test-logs/t095-roundtrip.log`
  - cleanup: `tmp/test-logs/t095-cleanup.log`
  - summary: `tmp/test-logs/t095-summary.log`

## Evidence

### 1. 009 example 启动了 2 个 ViewNode

- `tmp/test-logs/t095-status-before.log`
  - `status OK ... target_endpoint=127.0.0.1:8301`
  - `view_nodes=2`

### 2. 初始 status 能看到 Metadata leader 和 LIVE StorageNode

- `tmp/test-logs/t095-summary.log`
  - `status_before_summary=... target_endpoint=127.0.0.1:8301 ... metadata_nodes=3 storage_nodes=6`
  - `status_before_leader_hint=... leader_hint.endpoint=127.0.0.1:8401`
- `tmp/test-logs/t095-status-before.log`
  - 6 个 `storage_node ... liveness=live`

### 3. 停止的是 `view-1`

- `tmp/test-logs/t095-summary.log`
  - `failover_stop_line=[failover-view] stopped node_id=view-1 endpoint=127.0.0.1:8301`

### 4. surviving `view-2` 仍可完成 status

- `tmp/test-logs/t095-summary.log`
  - `failover_survivor_line=... surviving view ready node_id=view-2 endpoint=127.0.0.1:8302`
  - `status_after_target=127.0.0.1:8302`
  - `status_after_storage_live_count=6`
- `tmp/test-logs/t095-status-after-failover.log`
  - `status OK ... target_endpoint=127.0.0.1:8302`
  - `metadata_nodes=3`
  - `storage_nodes=6`
  - `leader_hint.endpoint=127.0.0.1:8401`

### 5. roundtrip 通过 surviving `view-2` 完成

- `tmp/test-logs/t095-summary.log`
  - `roundtrip_survivor_line=[roundtrip] confirmed surviving_view_endpoint=127.0.0.1:8302`
  - `roundtrip_verify_count=4`
- `tmp/test-logs/t095-roundtrip.log`
  - 先输出 `confirmed surviving_view_endpoint=127.0.0.1:8302`
  - 然后真实执行：
    - `upload OK`
    - `download`
    - `verify OK`
  - 覆盖真实 `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp` 路径

### 6. Metadata quorum 与 StorageNode discovery 未受 ViewNode failover 影响

- `tmp/test-logs/t095-status-after-failover.log`
  - `meta-1/meta-2/meta-3` 都是 `liveness=live`
  - leader hint 仍然存在
  - `store-1..store-6` 都是 `liveness=live`
- `tmp/test-logs/t095-roundtrip.log`
  - failover 后仍能发现 Metadata leader
  - failover 后仍能解析 `store-1..store-6` 的 data-plane endpoint

### 7. ViewNode 仍然只是 discovery / observation

- `tmp/test-logs/t095-status-after-failover.log`
  - `non_authority_boundary membership_observation_source=view_node raft_membership_authority=false object_manifest_authority=false`
- `tmp/test-logs/t095-roundtrip.log`
  - Metadata/Storage 诊断重复强调：
    - ViewNode 只是 discovery snapshot / leader hint 输入
    - MetadataService 仍是 authority

## Cleanup

- cleanup 方式：
  - 先调用 `examples/object-storage-local-009-dynamic/tingzhi.sh`
  - 再按 pid + ownership 兜底检查 `view-1` / `view-2` / `meta-4` / `meta-5` / `store-7`
- `tmp/test-logs/t095-cleanup.log` 记录：
  - `store-6 -> store-1` 已停止
  - `meta-3 -> meta-1` 已停止
  - `view-2` 已停止
  - `view-1` 因 failover 时已停，cleanup 为 `missing_pid_file`
- residual check：
  - 未发现本 example 残留进程

## Platform Notes

- Linux：`PASS`
- Windows：`pending`
- macOS：`pending`

## Result

- 最终状态：`PASS`
- 是否已勾选 `T095`：是
- 是否可以进入下一任务：可以，进入 `T096`
