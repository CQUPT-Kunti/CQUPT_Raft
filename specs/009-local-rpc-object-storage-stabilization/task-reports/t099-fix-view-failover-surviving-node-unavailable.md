# T099 Fix View Failover Surviving Node Incorrectly Reported As Unavailable

## 1. 根因是什么

- 根因不在 `modules/view/*` 的状态聚合实现。
- `ViewNodeRegistry::GetClusterView()` 仍按节点 `observed_at` 推导 `liveness`，没有把 peer sync 失败写成 self unavailable。
- 真正根因在 `examples/object-storage-local-009-dynamic/rpc_demo.sh` 的 failover 判定逻辑过严：
  - 先前错误要求固定基线 `metadata_nodes=3` / `storage_nodes=6`
  - 修正后又发现它仍把 `storage_nodes=0` 的 partial registry 观测当成 unavailable

## 2. 哪个状态传播链路导致 surviving ViewNode 被判定 unavailable

- 实际错误链路是脚本侧误判，不是服务端状态传播：
  - `run_failover_view()`
  - `status_reports_surviving_view_ready()`
  - 对 cluster shape 做过严 gate
  - 返回非零
  - 输出 `FAILED reason=surviving_view_status_unavailable`
- 当前确认不存在以下服务端传播链：
  - `peer sync failure -> available=false`
  - `peer unreachable -> node unavailable`
  - `cluster degraded -> node unavailable`

## 3. 修改了哪些文件

- `examples/object-storage-local-009-dynamic/rpc_demo.sh`
- `tests/view_failover_script_test.sh`
- `tests/CMakeLists.txt`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t099-fix-view-failover-surviving-node-unavailable.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 4. 修复后 peer sync failure 如何表现

- `view-2` 仍会继续输出：
  - `peer sync pull failed`
  - `peer sync push failed`
  - `peer sync backoff`
- 这些诊断没有被隐藏。
- peer sync failure 现在只表达为 peer sync 不健康 / degraded / partial，不再等价成 self unavailable。

## 5. 修复后单个存活 ViewNode 如何保持 available

- failover readiness 现在改为检查：
  - `status OK`
  - `target_endpoint` 已切到 surviving ViewNode
  - survivor 自身 `liveness=live`
  - survivor 自身 `health!=unavailable`
  - `non_authority_boundary` 仍存在
  - metadata discovery 仍有存活观测
- 同时明确允许：
  - storage registry 仍在收敛
  - `storage_nodes=0`
  - partial / degraded 视图

## 6. 新增或更新了哪些测试

- 保留并通过：
  - `ViewNodeDiscoveryTest.IntegrationFailoverDiscoveryUsesSurvivorObservedRegistryState`
- 新增并更新：
  - `ViewFailoverScriptValidation`
- 新增覆盖：
  - 动态扩容后 `metadata_nodes=5` / `storage_nodes=7`
  - survivor `health=degraded` 仍视为 ready
  - partial registry 场景下 `storage_nodes=0` 仍视为 ready
  - survivor `health=unavailable` 仍必须失败

## 7. 验证结果

- targeted build：
  - 命令：
    - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client ) 9>/tmp/cqupt_raft_build.lock`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t099-build.log`
- targeted CTest：
  - 命令：
    - `ctest --preset debug-tests -R "ViewFailover|FailoverView|ViewNode" --output-on-failure`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t099-ctest.log`
- 脚本级回归：
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t099-view-failover-script.log`
- 本地真实 failover example：
  - 命令序列：
    - `qidong.sh`
    - `rpc_demo.sh failover-view`
    - `tingzhi.sh`
  - 结果：`PASS`
  - 关键证据：
    - `tmp/test-logs/t099-failover-view.log`
    - 日志内可见 `surviving view ready node_id=view-2 endpoint=127.0.0.1:8302`

## 8. 是否 PASS / FAIL / SKIPPED

- `PASS`

## 9. 是否已勾选 T099

- 已勾选

## 10. 是否可以进入下一任务

- 可以进入下一任务

## 风险和后续注意事项

- 本任务修复的是 failover readiness 误判，不是修改 `modules/view` 的 authority / membership / learner / voter / dynamic join 逻辑。
- failover 后 registry 可能仍短时间处于 partial 状态；这在 009 语义上是允许的，只要 survivor 自身仍 live 且 status/discovery 还在。
- full dynamic sequence 的 clean rerun 仍可能受 example 运行态数据污染影响；该问题已单独记录在 `R23`。

## 跨任务风险更新

- 已同步更新 `cross-task-risk-notes.md`
  - `R22`：扩展为记录两类 failover false negative
    - 固定 `3/6` 基线误判
    - `storage_nodes=0` partial registry 误判
  - `R23`：保留 example 运行态污染风险
