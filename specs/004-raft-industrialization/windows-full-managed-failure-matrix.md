# Windows Full Managed Failure Matrix

## 目标

把 `T034` 首次 Windows full managed CTest sweep 的红灯收敛到单一矩阵，
并在每轮 focused / full managed rerun 后只保留“当前仍失败”的测试。

## 结果来源

- 首次 full managed sweep 历史日志：
  - `tmp/windows-release-managed-tests.log`
  - `tmp/test-ps1-managed.log`
- `T041` 收口日志：
  - `tmp/test-logs/t041-focused-runtime.log`
  - `tmp/test-logs/t041-focused-storage.log`
  - `tmp/test-logs/t041-windows-release-managed.log`
- exact seam 收口日志：
  - `tmp/test-logs/windows-exact-seam-build.log`
  - `tmp/test-logs/windows-exact-seam-focused.log`
  - `tmp/test-logs/windows-exact-seam-managed.log`

## 当前摘要

- Linux full managed CTest：`104/104` PASS
  - `platform-neutral`：100 tests PASS
  - `durability-boundary`：4 tests PASS
- Windows conservative baseline：`PASS`
  - `ctest --preset windows-release-tests`
  - 当前仍只覆盖 `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`
- Windows full managed：`PASS`
  - `ctest --preset windows-release-managed-tests`
  - 最近一次正式 rerun：`104/104` PASS
- Windows exact seam focused rerun：`PASS`
  - `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'`
  - 最近一次 focused rerun：`49/49` PASS

## 失败分类矩阵

| 分类 | 当前状态 | 失败数量 | 典型信号 | 对应后续任务 |
|------|----------|----------|----------|--------------|
| Windows full managed CTest entry / harness 问题 | 已收口 | 0 | 入口、discover、wrapper 当前无独立阻塞 | T036 |
| Windows runtime / timing / harness 问题 | 已收口 | 0 | Windows 长路径与等待条件噪声已收口 | T037 |
| Windows election / replication / commit-apply 红灯 | 已收口 | 0 | 原始 `4` 个 T038 项已转绿 | T038 |
| Windows snapshot / restart / catch-up 红灯 | 已收口 | 0 | 原始 `17` 个 T039 项已转绿或已被重新归类后收口 | T039 |
| Windows persistence / segment / storage 红灯 | 已收口 | 0 | 原始 `6` 个 T040 项已转绿 | T040 |
| Windows durability semantics adapt-or-defer | 已收口 | 0 | 原始 `9` 个 exact seam 已在 Windows 下稳定触发并转绿 | T041 |
| 其他 / 待进一步分类 | 当前无独立项 | 0 | 当前无剩余失败项 | T042 |

## 受管目标状态矩阵

| 受管目标 | 状态 | 失败数量 | 主要分类 | 后续任务 |
|----------|------|----------|----------|----------|
| `test_command` | PASS | 0 | N/A | N/A |
| `test_state_machine` | PASS | 0 | N/A | N/A |
| `test_min_heap_timer` | PASS | 0 | N/A | N/A |
| `test_thread_pool` | PASS | 0 | N/A | N/A |
| `test_kv_service` | PASS | 0 | N/A | N/A |
| `test_raft_election` | PASS | 0 | N/A | N/A |
| `test_raft_log_replication` | PASS | 0 | N/A | N/A |
| `test_raft_commit_apply` | PASS | 0 | N/A | N/A |
| `test_raft_split_brain` | PASS | 0 | N/A | N/A |
| `test_t017_leader_switch_ordering` | PASS | 0 | N/A | N/A |
| `persistence_test` | PASS | 0 | N/A | N/A |
| `snapshot_test` | PASS | 0 | N/A | N/A |
| `raft_integration_test` | PASS | 0 | N/A | N/A |
| `test_raft_snapshot_catchup` | PASS | 0 | N/A | N/A |
| `test_raft_snapshot_restart` | PASS | 0 | N/A | N/A |
| `test_raft_snapshot_diagnosis` | PASS | 0 | N/A | N/A |
| `test_raft_segment_storage` | PASS | 0 | N/A | N/A |
| `test_snapshot_storage_reliability` | PASS | 0 | N/A | N/A |
| `test_raft_replicator_behavior` | PASS | 0 | N/A | N/A |

## 当前失败详情

当前无失败项。

本文件不再保留已经转绿的 exact seam、platform-neutral storage、snapshot/restart、
election/replication 失败列表；这些项已在对应任务记录中留有收口摘要。

## 当前结论

- Windows full managed CTest 当前已达到 `104/104 PASS`
- Windows 当前无剩余 full managed 红灯
- 后续只剩 `T042` 的跨平台文档回填与最终任务收口，不再有待修失败矩阵
