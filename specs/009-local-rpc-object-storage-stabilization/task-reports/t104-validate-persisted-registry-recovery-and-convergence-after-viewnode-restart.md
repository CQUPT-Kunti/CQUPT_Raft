# T104 Validate Persisted Registry Recovery And Convergence After ViewNode Restart

## 1. 本任务做了什么

- 在 `tests/view_failover_test.cpp` 中新增 ViewNode restart recovery 回归测试。
- 用导出的旧 registry snapshot 显式模拟“持久化 registry 恢复”。
- 验证恢复后 peer sync 可以把恢复节点重新拉回到最新一致视图，且旧 registry 不会覆盖新状态。

## 2. 修改了哪些文件

- `tests/view_failover_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t104-validate-persisted-registry-recovery-and-convergence-after-viewnode-restart.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`

## 3. 新增或更新了哪些测试

- 新增：
  - `ViewFailoverTest.PersistedRegistryRecoveryReconvergesAfterViewNodeRestart`
- 保持回归：
  - `ViewFailoverTest.MultiViewSelfRefreshAndPeerSyncPreserveAvailabilityAcrossFailover`
  - `ViewFailoverTest.RecoveredViewNodePeerSyncReconvergesWithoutOverwritingLiveState`
  - `ViewFailoverTest.RegistryConvergesAcrossViewNodesAfterFailoverRecoveryAndPeerSync`
  - 既有 `ViewNodeDiscoveryTest.*`
  - `ViewFailoverScriptValidation`

## 4. 如何验证 registry recovery

- 先在 failover 前让 primary ViewNode 导出完整 registry snapshot，作为持久化 registry。
- primary 停机并重启后，先完成自身 register + self refresh，再显式导入旧 snapshot。
- 断言恢复后的 ViewNode 在未与 survivor peer sync 前，已经重新拿回旧 metadata/storage/view 记录，说明 registry 能从持久化状态恢复。
- 同时断言恢复节点自己的新 incarnation / sequence 保持为重启后的最新值，没有被恢复出来的旧 self state 覆盖。

## 5. 如何验证 restart 后的 convergence

- survivor 在 primary 宕机期间继续 self refresh，并接收更新后的 metadata/storage heartbeat。
- 重启后的 primary 先从旧 registry 恢复，再从 survivor 拉取 peer snapshot。
- 然后执行双向 peer sync，分别读取 survivor 与 recovered primary 的 `GetClusterView()`。
- 断言两个 ViewNode 最终都持有一致的：
  - `view_nodes`
  - `metadata_nodes`
  - `storage_nodes`
  - 主节点重启后的 self state
  - failover 期间 survivor 持续推进出来的 metadata/storage 最新状态

## 6. 如何验证 incarnation-aware merge

- 重启后的 primary 在完成 peer sync 并拿到最新状态后，再次导入 failover 前持久化下来的旧 registry snapshot。
- 断言导入结果：
  - `conflict_node_count == 0`
  - `stale_ignored_node_count == 4`
- 这说明：
  - 旧 primary self state 不会覆盖重启后的新 incarnation
  - survivor 的旧 sequence 不会覆盖 failover 后更新的 sequence
  - metadata/storage 的旧 incarnation/sequence 不会覆盖 failover 后的新 heartbeat

## 7. 验证结果

- 构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target view_failover_test test_view_node_discovery
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 测试：
  - `ctest --preset debug-tests -R "ViewNode|ViewFailover" --output-on-failure`
  - PASS
  - `41/41` tests passed

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T104

- 已勾选

## 10. 是否可以进入下一任务

- 可以

## 风险和后续注意事项

- 本次测试验证的是“恢复旧 registry snapshot 后的 merge 和最终收敛语义”，没有新增新的 ViewNode runtime persistence 功能。
- 当前回归覆盖证明：
  - 恢复节点允许短暂持有旧 registry
  - peer sync 能把它重新拉回最新状态
  - 旧状态不会回退 survivor 已经推进的新状态
- 仍需注意后续真正的 runtime durable load 路径必须复用相同的 snapshot 语义和 merge 顺序，不能另起一套恢复规则。
