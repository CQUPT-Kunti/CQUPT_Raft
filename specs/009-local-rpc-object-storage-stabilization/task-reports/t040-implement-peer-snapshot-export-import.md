# T040 Implement Peer Snapshot Export/Import

## Scope

本任务在 registry 层实现 peer snapshot export/import，只处理 ViewNode observed-state 同步，不实现后台 peer sync loop、failover 策略或 Raft membership 变更。

## Task Source

- `tasks.md`: T040
- `contracts/view-node-self-refresh-and-peer-sync.md`
- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`
- `modules/view/view_service_impl.cpp`
- `tests/view_node_discovery_test.cpp`

## Files Changed

- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`
- `modules/view/view_service_impl.cpp`
- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t040-implement-peer-snapshot-export-import.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

- 在 `view_registry.h/.cpp` 新增 registry 级 peer snapshot API：
  - `ViewRegistryPeerSnapshot`
  - `ExportPeerSnapshot(...)`
  - `ImportPeerSnapshot(...)`
  - 对应 request/result 结构与 import 计数
- `ExportPeerSnapshot(...)` 复用 cluster view 快照语义导出当前 observed registry snapshot，包含：
  - `cluster_id`
  - `generated_at_unix_ms`
  - `view_nodes`
  - `metadata_nodes`
  - `storage_nodes`
  - `leader_hint`
- `ImportPeerSnapshot(...)` 把 peer snapshot 重新映射回 registry observed-state 输入，并复用现有：
  - `RegisterNode(...)`
  - `HeartbeatNode(...)`
  - `DetermineObservedStateMergeDecision(...)`
  - `CompareIncarnationIds(...)`
- `view_service_impl.cpp` 的 Push/Pull peer sync RPC 现在只负责 proto <-> registry snapshot 转换，再调用 registry export/import；不再自己手写 register/heartbeat replay 逻辑。

## Boundary Checks

- 未实现 peer sync background loop。
- 未实现 multi-ViewNode failover 策略。
- 未修改 Raft membership / quorum。
- 未把 peer snapshot 当成 membership authority。
- 未让 `observed_time` 单独决定 merge 新旧。
- 未修改 example 脚本、proto 语义或 Storage/Metadata 动态加入逻辑。

## Import Ordering

`ImportPeerSnapshot(...)` 没有新造第二套 merge 规则，而是把每个 peer snapshot node 重新走一次 registry 既有输入路径：

1. `RegisterNode(...)` 负责 cluster/node_type/endpoint/data_dir_fingerprint 冲突检查与 sticky diagnostics。
2. `HeartbeatNode(...)` 负责 observed-state merge。
3. observed-state merge 继续由 `DetermineObservedStateMergeDecision(...)` 判定：
   - 高 incarnation 优先
   - 同 incarnation 下高 sequence 优先
   - `observed_time` 只能参与 stale 判定，不能单独覆盖更高 incarnation / sequence 的状态

因此：

- old incarnation peer snapshot 不能覆盖 local new incarnation
- low sequence peer snapshot 不能覆盖 higher sequence
- observed_time-only stale/dead peer snapshot 不能覆盖 newer live state

## Authority Boundary

- export/import 只处理 observed registry state。
- import 只回放 endpoint、health、capacity、load、metadata observed facts。
- imported `metadata.membership_state` 仍只是 observed fact，不是 committed membership authority。
- peer snapshot 不参与 voter/learner 变更、不参与 quorum 计算、不决定对象可见性。

## Tests Updated

新增或更新的关键测试：

- `ViewNodeDiscoveryTest.DualViewRegistrySyncPropagatesObservedStateToPeer`
  - 改为直接使用 registry `ExportPeerSnapshot(...)` + `ImportPeerSnapshot(...)`
- `ViewNodeDiscoveryTest.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState`
  - 改为直接走 registry import，验证 old incarnation 不能覆盖 newer live state
- `ViewNodeDiscoveryTest.PeerSnapshotLowerSequenceWithLaterObservedTimeCannotOverrideLiveState`
  - 新增，验证 same incarnation 下低 sequence + 更晚 observed_time + stale/dead facts 仍不能覆盖 newer live state
- `ViewNodeDiscoveryTest.PeerSnapshotImportConflictPreservesStickyDiagnostics`
  - 新增，验证 import 冲突诊断不会被吞掉，cluster view 仍保留 sticky diagnostics
- `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcExportsAndImportsObservedState`
- `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcOldIncarnationCannotOverrideNewerState`
  - 继续验证 RPC adapter 在切到 registry API 后不回归

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- CTest:
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
  - Summary: `28/28` PASS
- Test listing re-check:
  - `ctest --preset debug-tests -N`
  - Confirmed peer sync related entries include:
    - `ViewNodeDiscoveryTest.DualViewRegistrySyncPropagatesObservedStateToPeer`
    - `ViewNodeDiscoveryTest.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState`
    - `ViewNodeDiscoveryTest.PeerSnapshotLowerSequenceWithLaterObservedTimeCannotOverrideLiveState`
    - `ViewNodeDiscoveryTest.PeerSnapshotImportConflictPreservesStickyDiagnostics`
    - `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcExportsAndImportsObservedState`
    - `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcOldIncarnationCannotOverrideNewerState`
- Log files:
  - `tmp/test-logs/t040-build.log`
  - `tmp/test-logs/t040-ctest.log`
  - `tmp/test-logs/t040-ctest-list.log`

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated.
- Windows: not run, pending.
- macOS: not run, pending.

## Risks / Follow-ups

- T040 只完成 registry export/import 语义；后台 peer sync loop、retry/backoff 仍待 T041。
- 当前 peer snapshot 仍是 memory-to-memory observed-state sync；registry persistence / restart recovery 边界仍待 T042。
- RPC contract 已在 T039 存在；如果后续 proto 继续扩展，service/client 仍需保持对 registry export/import 的薄适配。

## Result

PASS

可以进入 T041。
