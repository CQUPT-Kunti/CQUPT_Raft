# T102 Add Multi-ViewNode Self-Refresh And Peer-Sync Failover Coverage

## 1. 本任务做了什么

- 在 `tests/view_failover_test.cpp` 中补充多 ViewNode 场景回归测试。
- 覆盖 self refresh、peer sync、failover、节点恢复后重新收敛之间的协同行为。
- 复跑现有 `ViewNode` / `ViewFailover` 回归集，确认旧语义没有退化。

## 2. 修改了哪些文件

- `tests/view_failover_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t102-add-multi-viewnode-self-refresh-and-peer-sync-failover-coverage.md`

## 3. 新增或更新了哪些测试

- 新增：
  - `ViewFailoverTest.MultiViewSelfRefreshAndPeerSyncPreserveAvailabilityAcrossFailover`
  - `ViewFailoverTest.RecoveredViewNodePeerSyncReconvergesWithoutOverwritingLiveState`
- 保持并回归验证：
  - 既有 `ViewFailoverTest.*`
  - 既有 `ViewNodeDiscoveryTest.*`
  - `ViewFailoverScriptValidation`

## 4. 如何验证 self refresh 与 peer sync 协同工作

- 用例先构造两个 ViewNode：
  - 一个节点持有本地 self refresh 后的最新 observed state
  - 另一个节点持有同一 peer 的较旧 registry 观测
- 通过 `PullPeerViewSnapshot` / `PushPeerViewSnapshot` 做 peer sync 后，断言：
  - 本地 self refresh 的 `incarnation_id`
  - `sequence`
  - `observed_at_unix_ms`
  - `liveness=live`
  仍然保留，不会被 stale peer snapshot 覆盖

## 5. 如何验证 failover 后状态能够正确收敛

- 在 peer sync 完成后停掉 primary ViewNode。
- surviving ViewNode 继续 self refresh，并接收 metadata/storage 新 heartbeat。
- 断言 failover 后：
  - survivor 保持 `live`
  - primary 只表现为 `dead`，不是 survivor `unavailable`
  - registry 中 metadata/storage 观测仍存在
- 再启动恢复后的 primary ViewNode，通过 peer sync 从 survivor 拉取 snapshot。
- 断言恢复节点带着新的 self refresh observed state 回归，并再次同步回 survivor，最终双方重新收敛到 `live`。

## 6. 如何验证 discovery/status 服务持续可用

- `GetClusterView()`：
  - failover 后继续返回 `kOk`
  - 恢复重收敛后继续返回 `kOk`
- `DiscoverMetadata()`：
  - failover 后仍返回可用 metadata
  - 恢复后仍保持可用
- `DiscoverStorage()`：
  - failover 后 survivor 仍可返回可用 storage
  - 恢复后仍可返回同步后的 storage 观测

## 7. 验证命令与结果

- 构建：
  - `cmake --build --preset debug-ninja-low-parallel --target view_failover_test test_view_node_discovery`
  - 结果：PASS
  - 日志：`tmp/test-logs/t102-view-build.log`
- 测试：
  - `ctest --preset debug-tests -R "ViewFailover|ViewNode" --output-on-failure`
  - 结果：PASS（`39/39`）
  - 日志：`tmp/test-logs/t102-view-ctest.log`

## 8. 结论

- 本任务状态：PASS
- T102 已勾选
- 可以进入下一任务

## 风险和后续注意事项

- 当前新增覆盖是 Linux targeted regression，不代表 Windows/macOS runtime 已验证。
- 当前新增覆盖验证了 failover 后的重收敛边界，但还没有覆盖长时间 multi-View soak、反复断连/回退/重连循环。
- 本任务未修改 ViewNode authority 边界、metadata membership、learner/voter、quorum、StorageNode dynamic join 逻辑。

## cross-task-risk-notes

- 未更新。
- 原因：本任务未发现新的跨任务风险类型；现有 `R11` 已覆盖 multi-View peer sync 的 soak / 非 Linux 运行时风险。
