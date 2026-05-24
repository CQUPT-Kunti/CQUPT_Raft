# T042 迁移剩余高价值 Raft 回归测试的 KV 断言到 metadata 断言

## 迁移范围
- 已迁移：
  - `tests/test_raft_split_brain.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/persistence_more_test.cpp`
- 已核查、无需改动：
  - `tests/test_raft_replicator_behavior.cpp`
  - `tests/test_raft_segment_storage.cpp`
  - 这两个文件本轮检查时已无 `CommandType::kSet/kDelete`、`DebugGetValue`、KV key/value 断言残留。

## 原 KV 断言与新 metadata 断言
- `test_raft_split_brain.cpp`
  - 原断言：`SetCommand(...)` 提案、`DebugGetValue()` 可见/缺失、`KvStateMachine` snapshot payload。
  - 新断言：
    - minority leader 超时后，`HeadObject` 不可见、`FindObject` 不存在、`object_index`/`chunk_ref_index` 不产生污染。
    - 已提交对象使用 `HeadObject + FindIndexedObjectId + FindChunkRefs + LastAppliedIndex` 验证多数派提交。
    - `InstallSnapshot` 改为 `MetadataStateMachine` V2 snapshot payload，校验 bucket 恢复、divergent suffix 被丢弃、replacement suffix 生效、`RequestCount` 与 `LastAppliedIndex/Term` 边界正确。
- `test_raft_snapshot_diagnosis.cpp`
  - 原断言：`WriteManyValues` + `DebugGetValue` 只看 value 可见性。
  - 新断言：
    - 批量写入改为 `CreateBucket`、`CreateObject`、`CommitObject`、`DeleteObject`。
    - restart/snapshot/corrupted snapshot/mismatched snapshot 场景使用 `MetadataRecoveryExpectation` 验证：
      - `request_table` 计数
      - `tombstone` 删除事实
      - `object_index` / `chunk_ref_index`
      - `last_applied_index` / `last_applied_term`
      - deleted object 不复活
      - committed object 的 chunk refs 可恢复
    - corrupted newest snapshot 场景额外保留“旧 snapshot 覆盖对象 + tail replay 对象 + tombstone + applied boundary”检查，避免只看对象可见性。
- `persistence_more_test.cpp`
  - 原断言：两阶段手工脚本里通过 `Propose(kSet)`、`DebugGetValue`、文本快照导出 KV。
  - 新断言：
    - 改为 metadata-only 两阶段手工恢复脚本。
    - 导出文本快照改为记录 `request_count`、`tombstone_count`、`last_applied_index/term`、对象可见性、内部 deleted/object_index/chunk_ref_index 状态。
    - phase-2 重启后先校验 phase-1 metadata 恢复，再追加 `recovery_probe` committed object。

## 本轮未迁移的 KV-only 内容
- 本轮目标 5 个文件内未保留 KV 断言残留。
- `persistence_more_test.cpp` 仍是 manual-only 入口，但已经不是 KV-only 内容；后续是否纳入 CMake/最终删除，不属于 T042。

## Linux 验证
- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_raft_split_brain test_raft_snapshot_diagnosis test_raft_replicator_behavior test_raft_segment_storage`
  - PASS
- `ctest --test-dir build/linux --output-on-failure -R "(RaftSplitBrain|RaftSnapshotDiagnosis)"`
  - PASS，11/11
- `ctest --test-dir build/linux --output-on-failure -R "RaftReplicatorBehavior"`
  - PASS，2/2
- `ctest --test-dir build/linux --output-on-failure -R "RaftSegmentStorage"`
  - PASS，19/19
- `/usr/bin/c++ ... -fsyntax-only tests/persistence_more_test.cpp`
  - PASS

## 剩余风险
- `test_raft_replicator_behavior` 中 `SlowFollowerDoesNotBlockMajorityCommit` 单次全量联跑时出现过一次时序波动，独立复跑与 suite 复跑均 PASS；本轮未改该文件，判定为现有集群时序抖动风险，不是本次 KV->metadata 迁移引入的新语义变化。
- `persistence_more_test.cpp` 仍不在 CTest target 中；本轮只做了语法检查与脚本语义迁移。
