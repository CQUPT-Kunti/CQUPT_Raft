# T103 Validate ViewNode Registry Convergence After Failover And Recovery

## 1. 本任务做了什么

- 在 `tests/view_failover_test.cpp` 中新增 registry convergence 回归测试。
- 验证多 ViewNode 在 failover、恢复、peer sync 双向同步后，最终观察到的 registry 视图能够收敛到一致状态。
- 复跑 `ViewNode|ViewFailover` 回归集，确认既有 self-refresh、peer-sync、failover 语义不退化。

## 2. 修改了哪些文件

- `tests/view_failover_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t103-validate-viewnode-registry-convergence-after-failover-and-recovery.md`

## 3. 新增或更新了哪些测试

- 新增：
  - `ViewFailoverTest.RegistryConvergesAcrossViewNodesAfterFailoverRecoveryAndPeerSync`
- 复跑并保持：
  - `ViewFailoverTest.MultiViewSelfRefreshAndPeerSyncPreserveAvailabilityAcrossFailover`
  - `ViewFailoverTest.RecoveredViewNodePeerSyncReconvergesWithoutOverwritingLiveState`
  - 既有 `ViewNodeDiscoveryTest.*`
  - `ViewFailoverScriptValidation`

## 4. 如何验证 registry convergence

- 先让 primary / survivor 拥有暂时不同的观察视图：
  - primary 持有 pre-failover registry snapshot
  - survivor 在 failover 后继续 self refresh，并接收更新后的 metadata/storage heartbeat
- 然后让恢复后的 primary 重新加入，并做双向 peer sync。
- 最终分别读取 survivor 和 recovered primary 的 `GetClusterView()`，逐项断言：
  - `view_nodes.size()`
  - `metadata_nodes.size()`
  - `storage_nodes.size()`
  - ViewNode observed state 的 `incarnation_id / sequence / observed_at`
  - metadata 的 `membership_epoch / observed_term`
  - storage 的容量与健康状态
  在两个 ViewNode 上一致。

## 5. 如何验证 failover 与恢复后的最终收敛行为

- failover 后允许出现短暂 registry 差异。
- survivor 持续 self refresh 和接收 heartbeat，形成新的最新状态。
- 恢复后的 primary 重新 self refresh 后，从 survivor 拉取 peer snapshot，再反向同步回 survivor。
- 断言最终：
  - primary 与 survivor 都恢复为 `live`
  - metadata/storage 观测都对齐到最新值
  - 最终 cluster view 在两个 ViewNode 上收敛一致

## 6. 如何验证 incarnation-aware merge 语义

- 在恢复后的 primary 上，显式再推送一份 failover 之前导出的旧 snapshot。
- 断言导入结果中至少存在 `stale_ignored_node_count >= 1`。
- 然后再检查最终 cluster view：
  - recovered primary 的新 `incarnation_id`
  - 新 `sequence`
  - 新 `observed_at_unix_ms`
  仍然保留，没有被旧 snapshot 覆盖。

## 7. 验证命令与结果

- 构建：
  - `cmake --build --preset debug-ninja-low-parallel --target view_failover_test test_view_node_discovery`
  - 结果：PASS
  - 日志：`tmp/test-logs/t103-view-build.log`
- 测试：
  - `ctest --preset debug-tests -R "ViewNode|ViewFailover" --output-on-failure`
  - 结果：PASS（`40/40`）
  - 日志：`tmp/test-logs/t103-view-ctest.log`

## 8. 结论

- 本任务状态：PASS
- T103 已勾选
- 可以进入下一任务

## 风险和后续注意事项

- 当前新增覆盖验证的是“最终收敛”语义，不要求 failover / recovery 过程中各 ViewNode 任意时刻完全一致。
- 当前仍未覆盖：
  - Windows/macOS runtime
  - 长时间 repeated peer disconnect / retry soak
  - registry 持久化恢复路径之外的更长链路 restart 行为
- 本任务未修改：
  - metadata membership
  - learner/voter
  - quorum
  - StorageNode dynamic join
  - ViewNode authority 边界

## cross-task-risk-notes

- 未更新。
- 原因：本任务未发现新的跨任务风险类型；现有多 ViewNode soak / 非 Linux 风险仍由既有风险项覆盖。
