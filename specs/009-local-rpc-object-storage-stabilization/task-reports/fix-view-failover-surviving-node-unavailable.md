# Fix View Failover Surviving Node Unavailable

## 1. 根因

- 根因不在 `modules/view` 的 `GetStatus()` / `GetClusterView()` / `ViewNodeStatus` 聚合逻辑。
- 根因在 `examples/object-storage-local-009-dynamic/rpc_demo.sh` 的 failover 校验脚本。
- `status_reports_live_cluster()` 把“cluster ready”硬编码成：
  - `metadata_nodes=3`
  - `storage_nodes=6`
- 当 full dynamic sequence 已经完成 `store-7` 加入与 `meta-4/meta-5` promote 后，真实 `status` 已变成：
  - `metadata_nodes=5`
  - `storage_nodes=7`
- 此时 surviving `view-2` 明明已经返回 `status OK`，脚本仍因为固定 `3/6` 断言失败而输出 `surviving_view_status_unavailable`。

## 2. 导致 surviving ViewNode 被判 unavailable 的链路

- 误判链路是：
  - `rpc_demo.sh failover-view`
  - `status_reports_surviving_view_ready()`
  - `status_reports_live_cluster()`
  - 固定检查 `metadata_nodes=3` / `storage_nodes=6`
  - 返回非零
  - `run_failover_view()` 输出 `FAIL reason=surviving_view_status_unavailable`
- 实际 009 原始失败日志里，survivor 侧已经同时满足：
  - `target_endpoint=127.0.0.1:8302`
  - `view-2 liveness=live`
  - `view-2 health=healthy`
  - `status OK`
- 因此这是脚本误判，不是 `peer sync_failed -> available=false` 的服务端状态传播。

## 3. 修改文件

- `examples/object-storage-local-009-dynamic/rpc_demo.sh`
- `tests/view_failover_script_test.sh`
- `tests/CMakeLists.txt`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/fix-view-failover-surviving-node-unavailable.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`

## 4. 修复后 peer sync failure 如何表现

- `peer sync connection refused`
- `peer sync backoff`

这些日志仍保留在 `view-2.log` 中，不做隐藏，也不做降级为成功。

- 修复后 failover 判定只要求：
  - survivor `target_endpoint` 已切到存活 ViewNode
  - survivor 自身 `liveness=live`
  - survivor 自身 `health!=unavailable`
  - 当前 `status` 摘要中的 metadata/storage live 数与当前摘要一致
- 因此 peer sync failure 现在表现为：
  - 可诊断的 peer sync 不健康
  - 但不再等价于 self unavailable

## 5. 修复后单个存活 ViewNode 如何保持 available

- `status_reports_surviving_view_ready()` 不再把“基线节点数 3/6”当成 availability 条件。
- 单个存活 ViewNode 只要还能：
  - 返回 `status OK`
  - 提供 survivor 自身 live 状态
  - 提供当前可见的 metadata/storage discovery 结果
就会被识别为 available。
- 这允许 failover 后出现：
  - `degraded`
  - `partial`
  - 观测面收敛中的 cluster view
- 但不会再把 survivor 直接判成 `unavailable`。

## 6. 新增或修改的测试

- 新增脚本级回归测试 `ViewFailoverScriptValidation`
  - 文件：`tests/view_failover_script_test.sh`
  - 覆盖：
    - `metadata_nodes=5` / `storage_nodes=7` 的 full dynamic 快照
    - survivor `health=degraded` 仍判定 ready
    - survivor `health=unavailable` 仍判定失败
- `tests/CMakeLists.txt` 已把该测试注册进 CTest。

## 7. 验证结果

- targeted configure：
  - `cmake --preset debug-ninja-low-parallel`
  - `PASS`
  - 日志：`tmp/test-logs/t098-fix-cmake-configure.log`
- targeted build：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client test_view_node_discovery ) 9>/tmp/cqupt_raft_build.lock`
  - `PASS`
  - 日志：`tmp/test-logs/t098-fix-build.log`
- targeted CTest：
  - `ctest --preset debug-tests -R "ViewFailover|FailoverView|ViewNode" --output-on-failure`
  - `PASS`
  - 日志：`tmp/test-logs/t098-fix-ctest.log`
- 真实 failover-only runtime：
  - `qidong.sh -> rpc_demo.sh failover-view -> rpc_demo.sh status -> rpc_demo.sh roundtrip -> tingzhi.sh`
  - `PASS`
  - 关键证据：
    - `tmp/test-logs/t098-fix3-failover-view.log`
    - `tmp/test-logs/t098-fix3-status-post-failover.log`
    - `tmp/test-logs/t098-fix3-roundtrip-post-failover.log`
- full dynamic rerun 复验过程中，出现了与本任务无关的旧运行态污染问题：
  - `meta-4` 启动时报 `candidate_raft_id already exists in committed voter set`
  - 见 `tmp/test-logs/t098-fix2-join-metadata-learner.log`
  - 已记录为跨任务风险 `R23`

## 8. 是否通过

- 本次根因定位与修复：`通过`
- failover 脚本误判修复：`通过`
- targeted CTest 回归：`通过`
- 真实 failover-only runtime：`通过`
- clean-state full dynamic sequence 再验证：`本次未完成，受 R23 影响`

## 9. 风险和后续注意事项

- 当前修复解决的是 failover 判定脚本的 false negative，不是新增 ViewNode 业务逻辑。
- failover 后 cluster view 可能短时间只收敛出 partial storage 视图；这符合 009 对 degraded / partial 的允许范围，只要 survivor 自身仍 live 且服务仍可用。
- 如果后续要重新 claim “full dynamic sequence 全量 PASS”，需要先清理 example 运行态数据，避免旧 committed membership 污染新的 learner join。

## 10. 跨任务风险更新

- 已更新 `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
  - `R22`：改为记录 failover false negative 的真实根因与脚本侧修复
  - `R23`：新增 example 运行态脏数据会污染 dynamic join / full-sequence rerun 的风险
