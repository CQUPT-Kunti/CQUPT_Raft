# T034 Record Linux Validation And Unresolved Peer-Sync Risks For ViewNode Observed-State Merge Ordering

## Scope

本任务是 Phase 4 ViewNode observed-state merge ordering 的验证收口任务。

本次不写生产代码，不改测试逻辑。重点是：

- 汇总 T026-T033 的当前完成状态
- 记录 Linux targeted validation 结果
- 记录当前仍未进入实现范围的 peer-sync / multi-ViewNode 风险
- 明确 Windows / macOS 仍为 pending / not run

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t034-record-linux-validation-and-unresolved-peer-sync-risks-for-viewnode-observed-state-merge-ordering.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Phase 4 Summary

按当前仓库实际状态，T026-T033 已完成并通过单 ViewNode 路径验证：

- T026: higher incarnation wins test
  - 已完成
  - `tests/view_node_discovery_test.cpp`
  - 当前覆盖：旧 incarnation 不能覆盖新 incarnation

- T027: same-incarnation higher sequence wins test
  - 已完成
  - 当前覆盖：同 incarnation 内更高 sequence 优先，晚到低 sequence 不得回滚

- T028: observed_time-only stale override rejection test
  - 已完成
  - 当前覆盖：`observed_time` 不能单独覆盖更高 sequence / 更高 incarnation 的现有状态

- T029: identity restart old-incarnation rejection test
  - 已完成
  - `tests/node_identity_test.cpp`
  - 当前覆盖：重启复用长期 `node_id`，拒绝旧 incarnation

- T030: incarnation-aware observed state support
  - 已完成
  - `modules/view/view_registry.h/.cpp`
  - 当前已有 `observed_state`、兼容字段和 registry snapshot 支撑

- T031: deterministic merge ordering
  - 已完成
  - 当前语义已按 incarnation 优先、同 incarnation 按 sequence 排序

- T032: conflict diagnostics
  - 已完成
  - duplicate `node_id` / `endpoint` / `data_dir_fingerprint` 当前已有诊断覆盖

- T033: service/client adapter mapping
  - 已完成
  - `view_service_impl.cpp` / `view_client.cpp`
  - 当前 service/client 已能保留 `incarnation_id`、`sequence`、`observed_at_unix_ms` 的关键观测事实

## Merge Ordering Semantics

当前 Phase 4 已收口出的语义是：

- 更高 incarnation 优先
- 同一 incarnation 内更高 sequence 优先
- `observed_time` 只用于 TTL / liveness / diagnostics，不单独决定覆盖顺序
- 旧 stale/dead 状态不能覆盖 newer live 状态
- duplicate `node_id` / `endpoint` / `data_dir_fingerprint` 冲突应诊断或拒绝，不能静默污染 registry
- ViewNode registry merge 仍然只是 discovery / observation，不是 Raft membership authority

## Linux Validation

### ViewNode merge ordering

构建命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock
```

测试命令：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
```

结果：

- build: PASS
- test: PASS
- `21/21` tests passed

日志：

- `tmp/test-logs/t034-build-view.log`
- `tmp/test-logs/t034-ctest-view.log`

重点覆盖的 Phase 4 / related cases：

- `ViewNodeDiscoveryTest.HigherIncarnationWinsForViewNodeObservedState`
- `ViewNodeDiscoveryTest.HigherSequenceWinsWithinSameIncarnation`
- `ViewNodeDiscoveryTest.ObservedTimeOnlyCannotOverrideHigherSequence`
- `ViewNodeDiscoveryTest.MissingIncarnationCannotOverrideIncarnationAwareCurrentState`
- `ViewNodeDiscoveryTest.IntegrationHeartbeatAdapterPreservesIncarnationAwareObservedState`
- `ViewNodeDiscoveryTest.HeartbeatConflictDiagnosticsDoNotOverrideExistingLiveState`

### Identity old-incarnation boundary

构建命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

测试命令：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\." --output-on-failure
```

结果：

- build: PASS
- test: PASS
- `35/35` tests passed

日志：

- `tmp/test-logs/t034-build-identity.log`
- `tmp/test-logs/t034-ctest-identity.log`

重点覆盖的 Phase 4 / related case：

- `NodeIdentityTest.RestartReusesNodeIdButRejectsOldIncarnation`

## Windows Validation

- Windows: pending / not run
- 原因：本任务未在 Windows 环境执行
- 未声明 PASS

## macOS Validation

- macOS: pending / not run
- 原因：本任务未在 macOS 环境执行
- 未声明 PASS

## Skipped Checks

- local RPC smoke: not run
  - 原因：本任务只做 Phase 4 merge ordering 的 targeted validation，不扩展到多进程 example

- peer sync runtime validation: pending
  - 原因：peer-sync 网络逻辑与 multi-ViewNode active-active 仍未进入本阶段实现范围

- 首次并发 identity build lock 获取:
  - 第一次与 `test_view_node_discovery` 并发请求同一个 build lock 时返回 `99`
  - 后续已顺序重试并完成 `test_node_identity` 构建与测试
  - 因此最终验证结果仍为 PASS，不计为 blocked

## Remaining Risks / Follow-ups

- peer sync 网络逻辑尚未实现
- multi-ViewNode active-active sync 仍待 Phase 5
- ViewNode failover discovery 仍待后续阶段
- 当前 Linux PASS 只覆盖 single-View RPC / registry path，不代表 multi-View runtime 已验证
- StorageNode dynamic join 不在本阶段
- Metadata learner join 不在本阶段
- Windows / macOS 仍为 pending / not run
- 后续新增 peer snapshot / push-pull RPC 时，必须保留 `incarnation_id`、`sequence`、stale/conflict diagnostics，不能在 adapter 层丢失这些状态

本次已同步文档：

- `validation-matrix.md`
  - 明确 single-View incarnation-aware merge ordering 已在 Linux targeted validation 中通过
  - 明确 multi-View peer sync runtime 仍 pending

- `cross-task-risk-notes.md`
  - 新增 R10，记录“当前只验证了 single-View RPC path，peer-sync 网络路径仍可能重新引入旧状态覆盖风险”

## Result

- Result: PASS
- `tasks.md`: 已仅将 T034 勾选完成
- 可以进入 T035
