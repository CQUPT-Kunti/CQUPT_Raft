# T044 Record Linux Validation And Skipped Platform Checks For Multi ViewNode Peer Sync

## Scope

本任务是 US2 多 ViewNode peer sync 的验证收口任务，只记录当前实现与验证状态，不写生产代码，不改测试逻辑。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t044-record-linux-validation-and-skipped-platform-checks-for-multi-viewnode-peer-sync.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## US2 Peer Sync Summary

- T035：已用 `ViewNodeDiscoveryTest.DualViewRegistrySyncPropagatesObservedStateToPeer` 锁定双 ViewNode registry sync 的 test-level 同步语义。
- T036：已用 `ViewNodeDiscoveryTest.IntegrationFailoverDiscoveryUsesSurvivorObservedRegistryState` 锁定 failover discovery 语义。
- T037：已用 `ViewNodeDiscoveryTest.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState` 锁定 old-incarnation rejection。
- T038：`cluster_config` 已支持 ViewNode `peer_seeds` 配置解析，单 ViewNode baseline 仍兼容。
- T039：已提供 `PullPeerViewSnapshot` / `PushPeerViewSnapshot` RPC contract 及 service/client adapter。
- T040：`ViewNodeRegistry` 已提供 peer snapshot export/import，并复用 deterministic merge ordering。
- T041：`apps/view_node_app.cpp` 已启动 peer sync loop，并带 retry/backoff 与安全停止。
- T042：当前 ViewNode registry restart recovery boundary 已明确为 memory-only，不宣称 registry 持久化恢复。
- T043：未新增独立 peer sync test target，当前 peer sync 测试继续复用 `test_view_node_discovery`。

## Current Peer Sync Semantics

- peer sync 只同步 ViewNode observed registry state。
- peer sync 不决定 Raft membership。
- peer sync 不决定 Metadata voter / learner membership。
- peer sync 不把 StorageNode registration 写入 Raft log。
- peer snapshot import 复用现有 deterministic merge ordering。
- old incarnation、same-incarnation lower sequence、以及 observed_time-only stale/dead state 都不能覆盖 newer live state。
- peer 不可达时记录诊断并走 retry/backoff，不阻塞 ViewNode 主服务。
- 当前 registry restart recovery 仍是 memory-only；重启后的 observed-state 重新收敛依赖 self refresh、register/heartbeat、以及运行期 peer sync。

## Linux Validation

- Build command:
  ```bash
  mkdir -p tmp/test-logs && (
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery > tmp/test-logs/t044-build.log 2>&1
  ) 9>/tmp/cqupt_raft_build.lock
  ```
- Build result: PASS
- Build log: `tmp/test-logs/t044-build.log`
- Test command:
  ```bash
  ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure > tmp/test-logs/t044-ctest.log 2>&1
  ```
- Test result: PASS
- Test summary: `ViewNodeDiscovery` `28/28` 通过
- Test log: `tmp/test-logs/t044-ctest.log`

本次 targeted Linux 验证覆盖了：

- dual ViewNode registry sync
- failover discovery
- peer snapshot old-incarnation rejection
- peer snapshot lower-sequence rejection
- peer sync RPC export/import
- self refresh / TTL / incarnation-aware ordering 回归

## Windows Validation

- Windows: pending / not run
- 原因：本任务未在 Windows 环境执行

## macOS Validation

- macOS: pending / not run
- 原因：本任务未在 macOS 环境执行

## Skipped Checks

- local RPC multi-View status smoke：未执行；本任务以 targeted `test_view_node_discovery` 为核心验证入口。
- local RPC roundtrip：未执行；不是 T044 核心要求。
- long-running multi-View peer sync soak：未执行；当前只做 targeted functional validation。

## Remaining Risks / Follow-ups

- 多 ViewNode 长时间运行 soak 仍未验证。
- Windows/macOS 平台上的 peer sync runtime 仍未验证。
- local RPC example 仍以单 ViewNode baseline 为主，多 ViewNode example/status 脚本增强留待后续阶段。
- StorageNode dynamic join 不在本阶段。
- Metadata learner join 不在本阶段。
- peer sync 必须继续保持 observed-state only 边界，后续不得把 ViewNode registry 演化成 membership authority。

## Result

- PASS
- 已在 `tasks.md` 勾选 T044
- 可以进入 T045
