# T027 迁移第二批 KV-based Raft 回归测试到 metadata 路径

## 结论

- T027 已完成。
- 本次选择迁移一组第二批代表性 Raft 回归测试：
  - `tests/test_t017_leader_switch_ordering.cpp`
- 该组测试已不再依赖 `KVCommand` / `SET` / `DEL` / `DebugGetValue()` / `kv={...}`。
- 本次同时扩展了 `tests/metadata_raft_test_utils.h`，并回归验证了 T026 已迁移的两组测试未被 helper 变更破坏。

## 选择范围

- 本次优先迁移 `leader switch + lagging follower catch-up` 场景。
- 原因：
  - 这组测试强依赖 Raft 行为本身
  - 仍属于相对低风险的 metadata 化迁移
  - 能覆盖 committed state 保留、delete 事实保留、follower catch-up、post-switch 新日志复制
- 本次未迁移：
  - `tests/raft_integration_test.cpp`
  - `tests/persistence_test.cpp`
- 原因：
  - `raft_integration_test.cpp` 仍包含 snapshot boundary 组合场景，适合与后续恢复类迁移一并处理
  - `persistence_test.cpp` 涉及 restart recovery / trusted boundary / persisted log 手工构造，风险更高

## 实际修改

- 更新 `tests/metadata_raft_test_utils.h`
  - 为已有 metadata 断言 helper 增加 `excluded nodes` 支持
  - 允许在 leader 停机、lagging follower 停机等场景下，只对存活节点验证 metadata 状态
- 重写 `tests/test_t017_leader_switch_ordering.cpp`
  - 删除 `CommandType::kSet / kDelete` payload
  - 删除 `SetCommand()` / `DeleteCommand()`
  - 删除 `WaitForValueOnAll()` / `WaitForMissingOnAll()` 中基于 `DebugGetValue()` 的 KV 断言
  - `ProposeWithRetry()` 改为提交 `MetadataCommand` 到 `RaftNode::ProposeMetadata(...)`
  - 新增 bucket/object 级 helper：
    - `ProposeCreateBucketWithRetry`
    - `ProposeCommittedObjectWithRetry`
    - `ProposeDeleteObjectWithRetry`
    - `WriteManyCommittedObjects`

## 迁移前后的断言变化

- `CommittedStateSurvivesLeaderSwitchAndNewLeaderContinuesReplication`
  - 原 KV 依赖：
    - `SET stable_before_switch`
    - `SET switch_anchor`
    - `SET after_switch`
    - `DebugGetValue()` 验证值是否存在
  - 迁移后：
    - `CreateBucket`
    - `CreateObject + CommitObject` 创建 `stable_before_switch`
    - `CreateObject + CommitObject` 创建 `switch_anchor`
    - leader 切换后继续 `CreateObject + CommitObject` 创建 `after_switch`
    - 断言：
      - `HeadObject` committed 可见
      - `object_index` 顺序一致
      - `ChunkRef` 可查询
      - `LastAppliedIndex` 与 commit/apply 边界推进一致
- `LaggingFollowerCatchesUpDuringLeaderSwitchWithoutCommitApplyReordering`
  - 原 KV 依赖：
    - 批量 `SET mixed_gap_*`
    - 对同一 key 做 `SET phase_1 -> SET phase_2 -> DEL`
    - `DebugGetValue()` 验证 delete 和 catch-up 结果
  - 迁移后：
    - 批量 `CreateObject + CommitObject mixed_gap_*`
    - `CreateObject + CommitObject mixed_ordering` 后执行 `DeleteObject`
    - leader 切换后再提交 `mixed_after_switch`、`mixed_tail`
    - 断言：
      - deleted object 的 `HeadObject` 返回 `NotFound`
      - deleted object 的内部 `ObjectRecord` 进入 `DELETED`
      - `object_index` / `chunk_ref_index` 已清理
      - lagging follower catch-up 后，旧 committed object、delete fact、post-switch committed object 同时成立
      - `ListObjects` 与 object index 一致

## 与恢复事实相关的覆盖

- 本次场景虽不直接构造持久化文件，但已覆盖与当前场景相关的恢复/一致性事实：
  - deleted object 不复活
  - `object_index` 与 `HeadObject/ListObjects` 一致
  - `chunk_ref_index` 在 delete 后被清理
  - `LastAppliedIndex` 随 leader switch 后的最后一条 metadata command 推进
- `request_table / tombstone / restart recovery` 的更深层恢复断言仍留待 `persistence_test.cpp` 和 snapshot/restart 任务迁移

## Linux 验证

- 选择原因
  - 本次修改只影响：
    - `test_t017_leader_switch_ordering.cpp`
    - `metadata_raft_test_utils.h`
  - 由于 helper 也被 T026 的两组测试复用，因此连同上一批回归一起验证
- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target test_t017_leader_switch_ordering test_raft_log_replication test_raft_commit_apply`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^(RaftLeaderSwitchOrderingTest|RaftLogReplicationTest|RaftCommitApplyTest)\."`
- 结果
  - configure：PASS
  - build：PASS
  - CTest：PASS
  - 合计 `6/6` 通过

## 日志

- `tmp/test-logs/t027-configure.log`
- `tmp/test-logs/t027-build.log`
- `tmp/test-logs/t027-ctest.log`

## 仍待迁移

- `tests/raft_integration_test.cpp`
- `tests/persistence_test.cpp`
- `tests/snapshot_test.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_snapshot_restart.cpp`

## 风险与范围

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 当前任务仅在 Linux 环境验证，Windows 留待后续 Windows 环境补测。
- 未删除 `KvStateMachine`。
- 未删除旧 KV Command 路径。
- 未修改 `RaftNode` 默认 wiring。
- 未进入 T028。

## 说明

- `tasks.md` 中现有 `T027` 仍指向另一项 US2 任务；为避免误标，本次未改 `tasks.md`。
