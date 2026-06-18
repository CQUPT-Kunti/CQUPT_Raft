# T037 Add Peer Snapshot Old-Incarnation Rejection Test

## Scope

本任务只新增 ViewNode peer snapshot old-incarnation rejection 测试，不修改生产 peer sync 逻辑、不修改 Raft membership。

## Task Source

- `tasks.md`: T037
- `tests/view_node_discovery_test.cpp`
- `modules/view/view_registry.h`
- `modules/view/view_registry.cpp`

## Files Changed

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t037-add-peer-snapshot-old-incarnation-rejection-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

在 `tests/view_node_discovery_test.cpp` 新增：

- `ViewNodeDiscoveryTest.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState`

测试先在本地 peer registry 中写入同一 `node_id` 的新 incarnation LIVE state，再构造来自 source registry 的旧 incarnation peer snapshot。该 peer snapshot 同时具备：

- 更旧的 `incarnation_id`
- 更高的 `sequence`
- 更晚的 `observed_time`
- source 侧 cluster view 中已判定为 `DEAD` 的 liveness

随后通过现有 `SyncSnapshotToPeerRegistry(...)` helper 将该 snapshot replay 到 peer registry，断言 peer registry 仍保留本地新 incarnation 的 LIVE state，且不会被旧 snapshot 的 observed_time 或 source 侧 dead 状态污染。

说明：

- 当前仓库还没有专门的 peer snapshot import API。
- 本测试使用现有 test-level snapshot replay helper 约束后续 T039/T040 必须复用相同的 deterministic merge ordering。
- 当前 replay helper 会重放 observed-state facts，并由接收侧本地重新计算 liveness；不会把 peer 的 liveness 直接当成 authority。

## Boundary Checks

- 未修改生产 peer sync / network 逻辑。
- 未修改 Raft membership / quorum。
- 未把 ViewNode 变成 membership authority。
- 未把 observed_time 变成旧 snapshot 覆盖新状态的唯一依据。
- 未新增全量构建要求。

## Validation

- Build command:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test listing:
  - `ctest --preset debug-tests -N`
  - Confirmed CTest entry:
    - `ViewNodeDiscoveryTest.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState`
- Test command:
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "ViewNodeDiscoveryTest\\.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Log files:
  - `tmp/test-logs/t037-build.log`
  - `tmp/test-logs/t037-ctest.log`

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated.
- Windows: not run, pending.
- macOS: not run, pending.

## Risks / Follow-ups

- 当前仍无生产级 peer snapshot import API；真正的 Push/Pull/Merge 路径仍待 T039/T040。
- 当前测试通过 `SyncSnapshotToPeerRegistry(...)` 锁定 merge ordering 契约，后续实现不得绕过该排序规则。
- peer snapshot liveness 目前不是接收侧 authority；接收侧仍按本地 `observed_time` 和 TTL 重新计算 liveness。

## Result

PASS

可以进入后续 peer sync 实现任务。
