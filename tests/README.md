# tests 目录说明

## 目标

说明 `tests/` 目录里哪些内容属于受管 GTest / CTest 回归，哪些属于
manual-only / diagnostic-only，哪些不会进入 Windows platform-neutral
fallback，以及 failure-injection / durability-boundary 测试应如何解释。

当前文档化验证范围只覆盖：

- Linux 主验证平台
- Windows 保守 fallback

macOS 当前不在本 feature 验证范围内。

编码注意事项速查见：[../specs/004-raft-industrialization/cross-platform-coding-notes.md](../specs/004-raft-industrialization/cross-platform-coding-notes.md)

## 1. 受管 GTest / CTest 回归

以下测试目标由 `tests/CMakeLists.txt` 通过 `add_raft_gtest(...)` 受管注册，
属于当前文档化的 GTest / CTest 回归入口：

- `test_command`
- `test_state_machine`
- `test_min_heap_timer`
- `test_thread_pool`
- `test_raft_election`
- `test_raft_log_replication`
- `test_raft_commit_apply`
- `test_raft_split_brain`
- `test_t017_leader_switch_ordering`
- `persistence_test`
- `snapshot_test`
- `raft_integration_test`
- `test_raft_snapshot_catchup`
- `test_raft_snapshot_restart`
- `test_raft_snapshot_diagnosis`
- `test_raft_segment_storage`
- `test_snapshot_storage_reliability`
- `test_raft_replicator_behavior`

这些目标构成当前维护者应优先使用的受管回归集合。

对应参与受管 GTest / CTest 的测试源文件是：

- `test_command.cpp`
- `test_state_machine.cpp`
- `test_min_heap_timer.cpp`
- `test_thread_pool.cpp`
- `test_raft_election.cpp`
- `test_raft_log_replication.cpp`
- `test_raft_commit_apply.cpp`
- `test_raft_split_brain.cpp`
- `test_t017_leader_switch_ordering.cpp`
- `persistence_test.cpp`
- `snapshot_test.cpp`
- `raft_integration_test.cpp`
- `test_raft_snapshot_catchup.cpp`
- `test_raft_snapshot_restart.cpp`
- `test_raft_snapshot_diagnosis.cpp`
- `test_raft_segment_storage.cpp`
- `test_snapshot_storage_reliability.cpp`
- `test_raft_replicator_behavior.cpp`

## 2. 受管回归的解释分层

### platform-neutral baseline

主要表示跨平台逻辑回归语义。当前受管测试里，以下目标可以进入这一层解释：

- `test_command`
- `test_state_machine`
- `test_min_heap_timer`
- `test_thread_pool`
- `test_raft_election`
- `test_raft_log_replication`
- `test_raft_commit_apply`
- `test_raft_split_brain`
- `test_t017_leader_switch_ordering`
- `snapshot_test`
- `raft_integration_test`
- `test_raft_snapshot_catchup`
- `test_raft_replicator_behavior`

注意：

- “带有 `platform-neutral` 标签”不等于“默认进入 Windows fallback”。
- Windows 默认只取其中更保守的 baseline 子集。

### shared restart / durability regression

以下目标包含 restart / trusted-state / durability-boundary 语义：

- `persistence_test`
- `snapshot_test`
- `test_raft_snapshot_restart`
- `test_raft_segment_storage`
- `test_snapshot_storage_reliability`

解释规则：

- 它们的恢复逻辑里有 platform-neutral 部分。
- 但只要碰到 exact durability、retained artifacts 或 crash-style 语义，
  解释应继续回到 Linux 主验收边界。

### Linux-primary / Linux-specific focus

以下目标或其中的测试场景，当前必须按 Linux 主环境解释：

- `persistence_test`
- `test_raft_snapshot_restart`
- `test_raft_snapshot_diagnosis`
- `test_raft_segment_storage`
- `test_snapshot_storage_reliability`

原因是它们涉及以下至少一种边界：

- `durability-boundary`
- `linux-specific-failure-injection`
- `linux-primary-diagnosis`
- retained-artifact diagnosis
- cluster/runtime-heavy 或更敏感的 Linux 主路径解释

## 3. Windows platform-neutral fallback 包含什么

当前 Windows fallback 入口是：

- `cmake --preset windows`
- `cmake --build --preset windows-release`
- `ctest --preset windows-release-tests`
- `.\test.ps1 -All`

当前默认只覆盖保守 baseline：

- `CommandTest`
- `KvStateMachineTest`
- `TimerSchedulerTest`
- `ThreadPoolTest`

也就是说，Windows fallback 当前只对应这些受管目标：

- `test_command`
- `test_state_machine`
- `test_min_heap_timer`
- `test_thread_pool`

这只是保守 platform-neutral baseline，不代表 Windows Raft 全功能测试通过。

## 3.1 Windows full managed CTest 入口

当前另外提供单独的 Windows full managed CTest 入口：

- `ctest --preset windows-release-managed-tests`
- `ctest --preset windows-debug-managed-tests`
- `.\test.ps1 -Managed`

解释规则：

- 这组入口和上面的 conservative baseline 分开存在。
- 它们运行完整受管 GTest / CTest 目标集合。
- 当前 Windows Release full managed 最新正式 rerun 已 `104/104 PASS`。
- 它们不等价于 Linux 当前 `104/104` managed 结果。
- 它们也不把 Linux-specific durability / failure-injection 改写成 Windows
  已等价验证。
- 当前完整状态只保留在
  [../specs/004-raft-industrialization/windows-full-managed-failure-matrix.md](../specs/004-raft-industrialization/windows-full-managed-failure-matrix.md)。

## 4. 哪些受管测试不进入 Windows fallback

以下受管回归当前都不属于 Windows platform-neutral fallback：

- `test_raft_election`
- `test_raft_log_replication`
- `test_raft_commit_apply`
- `test_raft_split_brain`
- `test_t017_leader_switch_ordering`
- `persistence_test`
- `snapshot_test`
- `raft_integration_test`
- `test_raft_snapshot_catchup`
- `test_raft_snapshot_restart`
- `test_raft_snapshot_diagnosis`
- `test_raft_segment_storage`
- `test_snapshot_storage_reliability`
- `test_raft_replicator_behavior`

因此：

- 不声明 Windows 已通过这些 Raft cluster / persistence / snapshot /
  durability 路径。
- 不声明 Windows 已等价验证 Linux-specific durability /
  failure-injection。

## 5. manual-only / diagnostic-only / temporary 文件

当前 `tests/` 目录里，不属于受管 GTest / CTest 回归入口、也不应作为正式验收
资产解释的文件有两类：

- manual-only / diagnostic-only：`persistence_more_test.cpp`
- temporary test file：`test_temp.cpp`

它们当前都没有在 `tests/CMakeLists.txt` 中通过 `add_raft_gtest(...)`
注册，因此都不参与 CTest，也不进入 Windows platform-neutral fallback。

### `persistence_more_test.cpp`

- `persistence_more_test.cpp`

它的角色是：

- 两阶段恢复演示程序
- 用于写入 marker、导出 manifest、保留恢复工件并做人工检查
- 不属于受管 CTest 回归
- 不属于 Windows platform-neutral fallback

其中适合受管回归的恢复场景已经迁入：

- `RaftSnapshotDiagnosisTest.RestartedSingleNodeLoadsSnapshotAndTailLogsWithoutPeers`
- `RaftSnapshotDiagnosisTest.CompactedClusterReplicatesNewLogAfterRestartedLeaderStepsDown`

### `test_temp.cpp`

`test_temp.cpp` 当前应解释为 temporary test file：

- 文件内部使用了 GTest 形式，但当前没有纳入 `tests/CMakeLists.txt`
- 不参与受管 CTest 回归
- 不属于 Windows platform-neutral fallback
- 不应作为正式验收资产或正式回归入口
- 如果后续需要清理、删除或迁移，应作为 follow-up 单独处理，不在当前任务执行

## 6. Linux 主入口与失败排障

当前 Linux 主入口是：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

当前 Linux 已知状态：

- `./test.sh --group persistence` 已通过
- `ctest --preset debug-tests --output-on-failure` 当前 `104/104` PASS
- Linux full managed CTest 当前已全绿：
  - `platform-neutral` 100 tests PASS
  - `durability-boundary` 4 tests PASS

失败后建议顺序：

1. 按原 group 低并发重跑：`CTEST_PARALLEL_LEVEL=1 ./test.sh --group <group>`
2. 需要保留现场时追加：`--keep-data`
3. 回看 `specs/004-raft-industrialization/validation-matrix.md`
   和 `platform-support.md` 的解释边界

`--keep-data` 的主要用途是保留：

- `raft_data/`
- `raft_snapshots/`
- `build/linux/tests/raft_test_data/`

## 7. CMake / GoogleTest 管理说明

当前 `tests/CMakeLists.txt` 的管理意图是：

1. 统一使用 `FetchContent` 拉取 GoogleTest，减少 Windows 下预编译 GTest
   带来的 Debug/Release 运行时不一致。
2. 在 MSVC 下设置 `CMAKE_MSVC_RUNTIME_LIBRARY`，让测试目标和 GoogleTest
   使用同一套运行时。
3. 通过 `gtest_discover_tests(...)` 给受管测试附加标签，区分
   `platform-neutral`、`durability-boundary`、
   `linux-specific-failure-injection`、`linux-primary-diagnosis`。

这些说明是构建与分类背景，不改变各测试的业务语义。
