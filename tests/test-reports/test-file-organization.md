# tests/ 重构报告

## 整体分析结论

- `metadata_state_machine_test.cpp` 超过 2200 行，混合了生命周期、查询、快照、并发、legacy compatibility 五类职责。
- `test_raft_snapshot_restart.cpp` 超过 1800 行，同时承载 restart 场景和 recovery/failure-injection 场景，主题过载。
- `tests/CMakeLists.txt` 对多源 gtest target 支持不足，导致大文件只能继续堆大。
- metadata 命令构造 helper 在 `metadata_state_machine_test.cpp` 与 `metadata_raft_test_utils.h` 重复。

## 本次拆分 / 抽取

- 将 `metadata_state_machine_test.cpp` 拆为：
  - `metadata_state_machine_test.cpp`：生命周期与状态冲突
  - `metadata_state_machine_query_test.cpp`：查询与 idempotency
  - `metadata_state_machine_snapshot_test.cpp`：snapshot round-trip / replay / corruption
  - `metadata_state_machine_concurrency_test.cpp`：并发 apply / query
  - `metadata_state_machine_legacy_test.cpp`：legacy strong-consistency compatibility
- 将 `test_raft_snapshot_restart.cpp` 拆为：
  - `test_raft_snapshot_restart.cpp`：restart 主流程
  - `test_raft_snapshot_recovery.cpp`：recovery / trusted snapshot / failure injection
- 新增 `tests/support/metadata_test_utils.h`，合并 metadata 命令构造与单机 snapshot 测试辅助。
- 新增 `tests/support/raft_snapshot_restart_test_utils.h`，承接 restart/recovery 共享 fixture、cluster 生命周期、等待与快照文件辅助。

## helper 合并结果

- `metadata_state_machine_test.cpp` 中本地的 create/delete/commit/abort command 构造已收敛到 `tests/support/metadata_test_utils.h`。
- `metadata_raft_test_utils.h` 复用同一套 metadata command builder，避免继续在 metadata Raft 回归测试中复制构造逻辑。
- `test_raft_snapshot_restart.cpp` 与 `test_raft_snapshot_recovery.cpp` 共享同一个 restart/recovery fixture 与等待 helper，不再在两个文件内复制 cluster 工具。

## 职责清晰化

- metadata 状态机测试按“生命周期 / 查询 / 快照 / 并发 / legacy”分层，后续新增 case 时可按主题落位。
- snapshot restart 主题与 snapshot recovery 主题分离后，失败定位不再混在一个超大文件里。
- `tests/support/` 成为共享 helper 边界，避免测试根目录继续堆积通用工具。

## AGENTS.md 分级更新

- 新增 `tests/AGENTS.md`，定义 tests/ 总入口、主题分布、拆分原则、CMake/CTest 规则。
- 新增 `tests/support/AGENTS.md`，约束共享 helper 的职责边界和文件归属。

## 覆盖不下降的原因

- 本次没有删除 gtest case，只做测试文件拆分和 helper 提取。
- `test_metadata_state_machine` 与 `test_raft_snapshot_restart` 仍保留原 target 名称与 label 语义，只是改为多源编译。
- 共享 helper 仅搬运命令构造、cluster 生命周期与等待逻辑，不改变断言条件。

## Linux 验证

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine test_raft_snapshot_restart`
- `ctest --test-dir build/linux/tests --output-on-failure -R \"(MetadataStateMachineTest|RaftSnapshotRestartTest|RaftSnapshotRecoveryTest)\"`
- `build/linux/tests/test_metadata_state_machine`
- `build/linux/tests/test_raft_snapshot_restart --gtest_filter='RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart'`

## 结果

- configure / build：PASS
- `test_metadata_state_machine`：PASS（33/33）
- `RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart`：PASS
- `ctest --test-dir build/linux/tests -R '(MetadataStateMachineTest|RaftSnapshotRestartTest|RaftSnapshotRecoveryTest)'`：
  - 44 项中 35 项通过
  - 9 项失败
  - 失败共性：原有 restart/recovery 场景仍通过 `SetCommand` / `DeleteCommand` 走 KV 命令路径，在当前 metadata-only 默认装配下统一报 `failed to parse metadata command`
  - 失败集中在：
    - `RaftSnapshotRestartTest.FollowerKeepsStateAfterInstallSnapshotAndRestart`
    - `RaftSnapshotRestartTest.LeaderKeepsCompactedSnapshotStateAfterRestart`
    - `RaftSnapshotRestartTest.FullClusterRestartsAfterSnapshotAndContinuesWriting`
    - `RaftSnapshotRecoveryTest.FullRestartReplaysSnapshotTailWithoutLosingDeletesOrOverwrites`
    - `RaftSnapshotRecoveryTest.RestartedFollowerAppliesCommittedTailExactlyOnceAfterSnapshotLoad`
    - `RaftSnapshotRecoveryTest.StandaloneRestartFallsBackToOlderTrustedSnapshotWhenNewestSnapshotIsCorrupted`
    - `RaftSnapshotRecoveryTest.RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`
    - `RaftSnapshotRecoveryTest.StandaloneRestartRejectsMetadataMismatchedVisibleSnapshotAndKeepsTrustedBoundary`
    - `RaftSnapshotRecoveryTest.AllPublishedSnapshotsInvalidYieldNoTrustedSnapshot`

## 剩余风险

- `test_raft_snapshot_catchup.cpp` 与 `test_raft_snapshot_diagnosis.cpp` 仍保留各自的 snapshot cluster helper 副本，后续若继续扩充该主题，建议继续合并到共享 helper。
- `persistence_test.cpp` 仍然偏大，但本次未对其做机械拆分，避免把持久化边界回归同时改得过宽。
- 上述 9 个 snapshot restart / recovery 用例仍依赖旧 KV 写路径；如果后续要把该主题彻底收敛到 metadata-only，需要单独迁移测试语义，而不是继续做文件级重构。
