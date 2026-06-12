# T054 Record Dynamic StorageNode Linux Validation

## Scope

本任务是 US3 StorageNode dynamic join 的验证收口任务，不写生产代码，不改测试逻辑。目标是记录 T045-T053 当前完成状态下的 Linux 定向验证结果，并明确 Windows / macOS 与 local RPC smoke 的 skipped / pending 边界。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t054-record-dynamic-storagenode-linux-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## US3 Summary

按当前仓库实际状态，T045-T053 已完成并形成以下闭环：

- T045：`storage_heartbeat_registry` 已覆盖运行中新增 StorageNode 注册进入 observed registry。
- T046：同一长期 `node_id` 的 StorageNode 重启后可用新 `incarnation_id` 重新注册，旧进程状态不会覆盖新进程。
- T047：duplicate `node_id` / duplicate `endpoint` / wrong-endpoint heartbeat 会返回冲突，且不污染已有 healthy record。
- T048：`IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest` 已证明新 StorageNode 只影响后续 placement，不自动改写旧对象 manifest。
- T049：StorageNode heartbeat payload 已补齐 capacity、load、disk pressure、health、writable、`incarnation_id`、`sequence` 等运行时事实。
- T050：`storage_node_app.cpp` 已接入 ViewNode seed list / first available 注册与 heartbeat failover，并复用持久 `node_id` + 当前 process incarnation。
- T051：ViewNode 合并后的 Storage observed state 已接入 placement candidate discovery，并按 live/healthy/writable/capacity 过滤。
- T052：现有 transfer path 保持兼容；upload 允许 future placement 使用动态 StorageNode；download 仍按 committed manifest。
- T053：Metadata manifest 获取路径已补 no-rebalance invariant diagnostics，并强化“新节点不进入旧 manifest”的集成回归断言。

补充说明：

- StorageNode identity load/create 不是 US3 新任务，但 Phase 2 的 T014 已提供当前 US3 所依赖的本地持久身份基线：首次缺失 `identity_file` 时创建身份，重启复用长期 `node_id`。

## Current StorageNode Dynamic Join Semantics

- StorageNode dynamic join 是 discovery / registration / heartbeat 路径，不进入 Raft log。
- StorageNode join 不影响 Raft quorum、election、Metadata voter / learner membership。
- ViewNode observed state 只提供 discovery facts，不是 Raft membership authority。
- StorageNode 使用本地持久 `node_id`，重启时复用长期身份，并生成新的 `incarnation_id`。
- ViewNode 合并后的 Storage observed state 会进入 placement candidate discovery。
- 只有 live、healthy、writable、未过期、容量满足约束的 StorageNode 才能进入正常写入候选。
- 动态加入的 StorageNode 只影响后续新对象 placement。
- 已 committed manifest 不因新 StorageNode 加入被重写。
- download 仍按 committed manifest 的 `replica_nodes` 读取。
- 009 当前不做旧对象 rebalance。

## Linux Validation

### Build

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_storage_heartbeat_registry test_integrated_object_storage_e2e > tmp/test-logs/t054-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

- Result: PASS
- Log: `tmp/test-logs/t054-build.log`

### Test

```bash
ctest --preset debug-tests -R '(^storage_heartbeat_registry$|^IntegratedObjectStorageE2ETest\.)' --output-on-failure > tmp/test-logs/t054-ctest.log 2>&1
```

- Result: PASS
- Log: `tmp/test-logs/t054-ctest.log`

### Verified Coverage

- `storage_heartbeat_registry`: PASS
- `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest`: PASS
- 其余命中的 `IntegratedObjectStorageE2ETest.*` 已启用用例：PASS
- 既有 disabled 用例保持 disabled，未被本任务改动：
  - `IntegratedObjectStorageE2ETest.AppConfigParsingSmokeCliOverridesMustRespectDurableIdentityAndStartupContracts`
  - `IntegratedObjectStorageE2ETest.HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage`
  - `IntegratedObjectStorageE2ETest.ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile`

Linux 定向验证结论：

- `storage_heartbeat_registry` 覆盖运行时注册、重启 incarnation 切换、冲突拒绝与 heartbeat observed-state 语义，当前通过。
- `integrated_object_storage_e2e` 覆盖 dynamic StorageNode placement、旧 manifest 不重写、manifest authority 仍来自 metadata committed state，当前通过。

## Windows Validation

- Windows: pending / not run
- 原因：本任务未在 Windows 环境执行，不能写 PASS。

## macOS Validation

- macOS: pending / not run
- 原因：本任务未在 macOS 环境执行，不能写 PASS。

## Skipped Checks

- local RPC status smoke：not run
  - 原因：T054 聚焦 US3 的 targeted Linux validation；当前 Phase 6 尚未提供真实“运行中 add-node”的 local RPC example 命令，相关多进程示例验证留给 Phase 10/T089-T098。
- local RPC roundtrip smoke：not run
  - 原因：不是本任务核心验证入口，且当前 example 仍主要覆盖 008 静态 baseline。
- Windows validation：not run
  - 原因：无 Windows 实机环境。
- macOS validation：not run
  - 原因：无 macOS 实机环境。

## Remaining Risks / Follow-ups

- 多 ViewNode peer sync 属于 US2/后续阶段能力，不在本任务验证范围内。
- 当前 US3 Linux PASS 主要来自 registry / placement / e2e 测试；真实 local RPC “运行中新增 StorageNode” 示例脚本仍待 Phase 10。
- 当前没有长时间 soak 来验证动态注册、心跳、placement 过滤在多进程运行中的稳定性。
- no-rebalance invariant 当前已有 e2e 回归和 metadata diagnostics，但未做长期 runtime soak。
- Windows / macOS 仍是 pending。
- Metadata learner join、learner catch-up、odd voter、batch promote 不在本任务范围内。

## Result

- PASS
- 已在 `tasks.md` 中只勾选 T054。
- 可以进入 T055。
