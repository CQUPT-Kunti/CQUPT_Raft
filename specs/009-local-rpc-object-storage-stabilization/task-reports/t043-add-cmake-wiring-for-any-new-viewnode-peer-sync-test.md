# T043 Add CMake Wiring For Any New ViewNode Peer Sync Test

## Scope

本任务只检查 ViewNode peer sync 相关测试是否新增了独立测试文件或独立 target，并确认 `tests/CMakeLists.txt` 的 CMake / CTest wiring 是否需要补充。

## Task Source

- `tasks.md`: T043
- `tests/CMakeLists.txt`
- `tests/view_node_discovery_test.cpp`
- Phase 5 已完成的 T035-T040 测试现状

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t043-add-cmake-wiring-for-any-new-viewnode-peer-sync-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

本任务结论是 no-op：

- 未发现新增独立的 ViewNode peer sync 测试文件。
- 当前仍只有 `tests/view_node_discovery_test.cpp`。
- Phase 5 新增/更新的 peer sync 相关测试都继续挂在已有 target `test_view_node_discovery` 下。
- 因此不需要新增 test target，也不需要修改 `tests/CMakeLists.txt`。

## Current Test Entry

- CMake target:
  - `test_view_node_discovery`
- 对应 CMake wiring:
  - `add_raft_gtest(test_view_node_discovery view_node_discovery_test.cpp "${RAFT_008_LABELS_VIEW_NODE}")`
- 当前 peer sync 相关 CTest names 仍然在同一个 target 下，包括：
  - `ViewNodeDiscoveryTest.DualViewRegistrySyncPropagatesObservedStateToPeer`
  - `ViewNodeDiscoveryTest.PeerSnapshotOldIncarnationCannotOverrideLocalNewLiveState`
  - `ViewNodeDiscoveryTest.PeerSnapshotLowerSequenceWithLaterObservedTimeCannotOverrideLiveState`
  - `ViewNodeDiscoveryTest.PeerSnapshotImportConflictPreservesStickyDiagnostics`
  - `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcExportsAndImportsObservedState`
  - `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcOldIncarnationCannotOverrideNewerState`

## Label Check

- 未新增独立 target，因此没有新增 label wiring。
- 当前 `test_view_node_discovery` 继续使用 `RAFT_008_LABELS_VIEW_NODE`。
- 根据 `tests/CMakeLists.txt`，该 label 集合为：
  - `integrated-object-storage`
  - `view-node`
  - `platform-neutral`

## Boundary Checks

- 未修改生产代码。
- 未修改测试断言。
- 未新增无必要的 CMake target。
- 未把 unrelated tests 拉入 peer sync target。
- 未执行全量构建。

## Validation

- 文件检查：
  - `rg --files tests | rg "view_node.*test\\.cpp|peer_sync|peer.*sync|view.*peer.*test"`
  - 结果：仅确认 `tests/view_node_discovery_test.cpp`
- CTest entry listing：
  - `ctest --preset debug-tests -N`
  - 结果：确认上述 peer sync 相关 CTest names 仍在 `ViewNodeDiscoveryTest.*`
- Build：
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test：
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
  - Summary: `28/28` PASS
- Logs:
  - `tmp/test-logs/t043-build.log`
  - `tmp/test-logs/t043-ctest.log`
  - `tmp/test-logs/t043-ctest-list.log`

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated.
- Windows: not run, pending.
- macOS: not run, pending.

## Risks / Follow-ups

- T043 只确认当前 peer sync 测试继续复用 `test_view_node_discovery`，不新建 target。
- 如果 T044 之后新增独立 `tests/view_node_peer_sync_test.cpp` 或拆分 target，才需要回到 `tests/CMakeLists.txt` 补接线。

## Result

PASS

可以进入 T044。
