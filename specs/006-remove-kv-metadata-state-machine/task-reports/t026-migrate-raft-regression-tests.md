# T026 迁移第一批 Raft 回归测试到 MetadataStateMachine / MetadataCommand

## 结论

- T026 已完成。
- 本次迁移了两组低风险、代表性的 KV-based Raft 回归测试：
  - `RaftLogReplicationTest`
  - `RaftCommitApplyTest`
- 被迁移测试已不再依赖 `KVCommand` / `SET` / `DEL` / `DebugGetValue()`
- 断言已改为 `MetadataCommand` 提案 + `MetadataStateMachine` 查询/索引/删除态验证
- `KvStateMachine` 和旧 KV Command 路径仍保留，供后续未迁移回归测试继续使用

## 实际修改

- 新增测试 helper：
  - `tests/metadata_raft_test_utils.h`
  - 提供 metadata command builder：
    - `MakeCreateBucketCommand`
    - `MakeCreateObjectCommand`
    - `MakeCommitObjectCommand`
    - `MakeDeleteObjectCommand`
  - 提供 metadata 回归断言 helper：
    - `ProposeMetadataCommand`
    - `WaitUntilAllCommittedObject`
    - `WaitUntilAllDeletedObjectHidden`
    - `WaitUntilAllListObjectsMatch`
- 迁移 `tests/test_raft_log_replication.cpp`
  - 原依赖：
    - `CommandType::kSet`
    - `leader->Propose(cmd)`
    - `Describe()` 中 `kv={...}` 片段
  - 迁移后：
    - 通过 `CreateBucket -> CreateObject -> CommitObject` 产生 Raft 日志
    - 仍保留 `last_log_index / commit_index / last_applied` 的 Raft 边界断言
    - 用 `MetadataStateMachine::HeadObject` / `FindIndexedObjectId` / `FindChunkRefs` 验证 committed object
    - 用 `MetadataStateMachine::ListObjects` 验证稳定顺序与对象索引一致性
- 迁移 `tests/test_raft_commit_apply.cpp`
  - 原依赖：
    - `CommandType::kSet`
    - `CommandType::kDelete`
    - `Describe()` 中 `kv={...}` 与空 KV 断言
  - 迁移后：
    - `CommitAndApplyIndexesAdvanceAfterSuccessfulPropose`
      - 使用 `CreateBucket -> CreateObject -> CommitObject`
      - 断言 committed object 可见、chunk refs 可见、`LastAppliedIndex` 跟进
    - `DeleteCommandIsAppliedToAllNodes`
      - 使用 `CreateBucket -> CreateObject -> CommitObject -> DeleteObject`
      - 先确认 committed object 已在所有节点可见
      - 再确认 `HeadObject` 返回 `NotFound`
      - 再确认 `FindObject` 处于 `DELETED`
      - 再确认 `FindIndexedObjectId` / `FindChunkRefs` 已清理
      - 再确认 `ListObjects` 不再暴露该对象

## 迁移后的断言模型

- 不再断言 `kv={x=1}`、`kv={}` 之类 KV 视图
- 改为断言以下 metadata 状态：
  - bucket 存在且 active
  - object committed 后可被 `HeadObject` 查询
  - object committed 后在 `object_index` 中可见
  - committed object 的 `ChunkRef` 可查询
  - deleted object 不再被 `HeadObject` / `ListObjects` 暴露
  - deleted object 的内部 `ObjectRecord` 仍保留 `DELETED` 终态
  - `LastAppliedIndex` 随最终 metadata command 推进

## 仍待迁移的测试

- `tests/test_t017_leader_switch_ordering.cpp`
- `tests/raft_integration_test.cpp`
- `tests/persistence_test.cpp`
- `tests/snapshot_test.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_snapshot_restart.cpp`

## 本次未迁移原因

- 上述测试涉及：
  - leader switch / follower catch-up
  - restart recovery
  - snapshot / replay / boundary clamp
  - 更复杂的“删除后不复活 / request_table / tombstone / replay 边界”组合断言
- 这些场景风险更高，适合在后续 T027+ 的恢复类/切主类任务里分批迁移

## Linux 验证

- 选择原因
  - 本次只改测试文件和测试 helper，没有改业务逻辑、默认 wiring 或 CMake target
  - 因此执行 configure + 受影响测试 target build + 受影响 CTest filter 的最小闭环
- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication test_raft_commit_apply`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^(RaftLogReplicationTest|RaftCommitApplyTest)\."`
- 结果
  - configure：PASS
  - build：PASS
  - CTest：PASS
  - 合计 `4/4` 通过

## 日志

- `tmp/test-logs/t026-configure.log`
- `tmp/test-logs/t026-build.log`
- `tmp/test-logs/t026-ctest.log`

## 风险与范围

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 当前任务仅在 Linux 环境验证，Windows 留待后续 Windows 环境补测。
- 未删除 `KvStateMachine`。
- 未删除旧 KV Command 路径。
- 未修改 `RaftNode` 默认 wiring。
- 未进入 T027。

## 说明

- `tasks.md` 中现有 `T026` 仍指向另一项 US2 任务；为避免误标，本次未改 `tasks.md`。
