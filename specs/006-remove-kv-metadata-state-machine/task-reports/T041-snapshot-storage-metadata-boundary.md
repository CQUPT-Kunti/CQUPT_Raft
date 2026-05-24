# T041 SnapshotStorage metadata boundary

## 结论
- `SnapshotStorage` 的持久化语义保持不变，没有弱化 staging publish、checksum、Linux file fsync、Linux directory fsync、Windows `FlushFileBuffers`。
- 本次只增强诊断与对应测试，不改 `MetadataStateMachine` snapshot V2 格式，不改 `RaftNode` replay 边界。

## 持久化语义是否保持
- `SaveSnapshotFile()` 仍然采用：
  - root/staging 目录创建
  - staging 目录内写 `data.bin`
  - checksum 计算
  - staging 目录内写 `__raft_snapshot_meta`
  - file flush/sync
  - staging 目录 sync
  - rename/replace 到 final snapshot dir
  - root 目录 sync
- Linux 路径：
  - `SyncFile()` 仍使用真实 `fsync(fd)`
  - `SyncDirectory()` 仍使用真实目录 `fsync(fd)`
- Windows 路径：
  - 文件与目录仍通过 `CreateFileW` + `FlushFileBuffers`
  - 未改 replace/rename 语义
- 没有引入 direct-write final snapshot file，也没有用 silent fallback 绕过 publish/sync 失败。

## 新增/增强的 metadata-only 诊断
- published snapshot 目录缺少 meta：
  - 现在明确报 `snapshot publish incomplete: meta file missing`
- published snapshot 目录缺少 data：
  - 现在明确报 `snapshot publish incomplete: data file missing`
- outer snapshot meta header：
  - 区分 `corrupted snapshot meta header`
  - 区分 `snapshot meta magic mismatch`
  - 区分 `snapshot meta version mismatch`
  - 区分 `snapshot meta missing data file name`
- metadata snapshot V2 `data.bin` fixed header：
  - 识别 `MDS2` magic 后，额外读取 V2 fixed header
  - 若 header 不完整，明确报 `metadata snapshot header truncated or unreadable`
  - 若 version 不匹配，明确报 `metadata snapshot version mismatch`
  - 若 `data.bin` 内 `last_applied_index/term` 与 outer meta `last_included_index/term` 不一致，明确报 `metadata snapshot boundary mismatch`
- 既有诊断保持：
  - `staging snapshot directory ignored`
  - `snapshot checksum mismatch`
  - 各类 failure injection seam 的 publish/directory-sync/prune 诊断

## 修改文件
- `modules/raft/storage/snapshot_storage.cpp`
- `tests/test_snapshot_storage_reliability.cpp`
- `tests/test_raft_snapshot_recovery.cpp`

## Linux 验证
- 构建：
  - `cmake --build --preset debug-ninja-low-parallel --target test_snapshot_storage_reliability snapshot_test test_raft_snapshot_restart test_raft_snapshot_catchup test_metadata_recovery_stress`
  - 结果：PASS
- 测试：
  - `build/linux/tests/test_snapshot_storage_reliability --gtest_filter='SnapshotStorageReliabilityTest.*'`
  - `build/linux/tests/test_metadata_recovery_stress --gtest_filter='MetadataRecoveryStressTest.*'`
  - `build/linux/tests/test_raft_snapshot_restart --gtest_filter='RaftSnapshotRestartTest.SnapshotAndPostSnapshotLogsRecoverAfterFullRestart'`
  - `build/linux/tests/test_raft_snapshot_catchup --gtest_filter='RaftSnapshotCatchupTest.FollowerContinuesReplicatingLogsAfterInstallingSnapshot'`
  - `build/linux/tests/snapshot_test --gtest_filter='RaftSnapshotRecoveryTest.SavesSnapshotAndRestoresAfterRestart'`
  - 结果：PASS

## Windows 影响与风险
- 代码路径上未改 Windows `SyncFile()` / `SyncDirectory()` 的 `FlushFileBuffers` 调用，也未改 Windows rename/publish 顺序。
- 因此 Windows durability contract 语义应与修改前一致。
- 但本次未做 Windows 实测，只能确认代码路径未弱化，不能报告 Windows PASS。
- 后续用户补测时，建议重点覆盖：
  - published dir 缺 meta/data 的诊断文案
  - metadata snapshot V2 version/header/boundary mismatch 诊断
  - `FlushFileBuffers` 失败路径是否仍返回明确错误
