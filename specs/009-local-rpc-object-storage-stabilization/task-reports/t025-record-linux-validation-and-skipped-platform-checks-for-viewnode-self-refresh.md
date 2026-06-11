# T025 Record Linux Validation And Skipped Platform Checks For ViewNode Self Refresh

## Scope

本任务是 Phase 3 ViewNode self refresh 的验证记录任务。

本任务不写生产代码，不改测试逻辑，不改协议，不改 example 脚本，只记录：

- T018-T024 在 Linux 上的 targeted validation 结果
- 本任务补做的 `test_view_node_discovery` 验证
- 本任务补做的 local RPC `status` smoke
- Windows / macOS 的 pending / not run 状态
- 当前仍保留的风险和后续阶段边界

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t025-record-linux-validation-and-skipped-platform-checks-for-viewnode-self-refresh.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Phase 3 Summary

按当前仓库实际状态，Phase 3 的 ViewNode self refresh 相关任务已经收口到以下状态：

- T018：`ViewNodeSelfRefreshKeepsSelfLiveBeyondDeadTtl` 测试已存在，并已固定 self refresh keep-live 语义
- T019：`ViewNodeSelfRefreshDisabledAllowsTtlTransitions` 测试已存在，并证明停 refresh 后 TTL 仍正常降级
- T020：local RPC `rpc_demo.sh status-self-liveness` regression 入口已存在，可暴露 self-liveness 问题
- T021：registry self refresh update path 已存在
- T022：`apps/view_node_app.cpp` 已接入 self refresh loop 和 clean shutdown 语义；本次 local RPC smoke 看到 `view-1 last_sequence=3`，说明 loop 在运行
- T023：self refresh payload 在 registry/snapshot 层已包含 `node_id`、`endpoint`、`incarnation_id`、`last_sequence`、`last_seen_unix_ms`、`health`、`liveness`
- T024：cluster view diagnostics 已能输出 `self_refresh_state`，包含 `source / node_id / endpoint / incarnation / sequence / last_seen_unix_ms / health / liveness`

补充说明：

- `task-reports/` 目录下未找到单独的 T022 任务报告文件，但 `tasks.md` 当前已将 T022 标记为完成，且本次 Linux smoke 已验证 app self refresh loop 的实际行为。

## Linux Validation

### Targeted Build

命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
) 9>/tmp/cqupt_raft_build.lock
```

结果：

- PASS

日志：

- `tmp/test-logs/t025-build.log`

### Targeted Test

命令：

```bash
ctest --preset debug-tests -R "ViewNodeDiscoveryTest\\." --output-on-failure
```

结果：

- PASS
- `15/15` tests passed

覆盖到的关键 self refresh 相关测试包括：

- `ViewNodeDiscoveryTest.ViewNodeSelfRefreshKeepsSelfLiveBeyondDeadTtl`
- `ViewNodeDiscoveryTest.SelfRefreshPayloadIncludesIncarnationSequenceObservedTimeHealthAndEndpoint`
- `ViewNodeDiscoveryTest.IntegrationClusterViewExposesSelfRefreshSequenceLivenessDiagnostics`
- `ViewNodeDiscoveryTest.ViewNodeSelfRefreshDisabledAllowsTtlTransitions`

日志：

- `tmp/test-logs/t025-ctest.log`

### Local RPC Status Smoke

本任务执行了轻量 local RPC `status` smoke，不执行 roundtrip。

命令：

```bash
examples/object-storage-local-3meta-6store/tingzhi.sh
examples/object-storage-local-3meta-6store/qidong.sh
examples/object-storage-local-3meta-6store/rpc_demo.sh status
examples/object-storage-local-3meta-6store/tingzhi.sh
```

结果：

- `qidong.sh`: PASS
- `rpc_demo.sh status`: PASS
- `tingzhi.sh`: PASS

观测到的关键结果：

- `status OK`
- `view_nodes=1 metadata_nodes=3 storage_nodes=6`
- `view_node node_id=view-1 ... liveness=live ... last_sequence=3`
- diagnostics 中出现：
  - `self_refresh_state source=self_refresh`
  - `incarnation=...`
  - `sequence=3`
  - `health=healthy`
  - `liveness=live`

这说明当前 Linux 本地 smoke 已经能在真实 example/status 层看到：

- ViewNode 自身 liveness
- self refresh sequence
- self refresh diagnostics

日志：

- `tmp/test-logs/t025-start.log`
- `tmp/test-logs/t025-status.log`
- `tmp/test-logs/t025-stop-before.log`
- `tmp/test-logs/t025-stop-after.log`
- `tmp/test-logs/t025-smoke.rc`

## Windows Validation

- Windows: pending / not run
- 原因：本任务未在 Windows 环境执行
- 不写 PASS

## macOS Validation

- macOS: pending / not run
- 原因：本任务未在 macOS 环境执行

## Skipped Checks

本任务明确 skipped 的项目如下：

- Windows validation
  - 原因：无 Windows 实机环境
- macOS validation
  - 原因：无 macOS 实机环境
- local RPC roundtrip
  - 原因：T025 只需要收口 ViewNode self refresh 的 Phase 3 验证，不要求 upload/download/cmp
- local RPC `status-self-liveness` 长时 TTL regression 重跑
  - 原因：该回归入口已在 T020 单独验证并记录；本任务只补充当前修复后的短 smoke `status`

## Remaining Risks / Follow-ups

当前仍保留的风险和后续边界：

- ViewNode peer sync 仍未实现，属于 Phase 5 后续任务
- old incarnation / multi-view merge safety 的完整收口仍待 Phase 4 / Phase 5
- multi ViewNode failover 仍待后续阶段
- StorageNode dynamic join 不在本阶段
- Metadata learner join / odd voter 不在本阶段
- Windows / macOS 尚未验证
- local RPC 长时间 self refresh soak 尚未验证
- 断电级 durability / registry 持久化边界不在当前 Phase 3 范围
- T022 的单独任务报告文件目前未在 `task-reports/` 目录中找到，当前只靠代码路径、`tasks.md` 状态和本次 Linux smoke 交叉确认

## Result

- 结果：PASS
- 已在 `tasks.md` 中仅将 T025 从 `[ ]` 勾选为 `[X]`
- 可以进入 T026
