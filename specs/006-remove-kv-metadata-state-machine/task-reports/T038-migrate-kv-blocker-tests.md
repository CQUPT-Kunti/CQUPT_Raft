# T038 迁移剩余 KV blocker 测试到 metadata 路径

## 1. 本轮迁移范围

- 已迁移 blocker：
  - `tests/test_raft_replicator_behavior.cpp`
  - `tests/test_raft_segment_storage.cpp`
  - `tests/metadata_raft_test_utils.h`（新增通用 metadata proposal helper）
- 未迁移、仅标记为后续删除候选：
  - `tests/test_command.cpp`
  - `tests/test_state_machine.cpp`

## 2. 迁移内容

### 2.1 `test_raft_replicator_behavior.cpp`

- 删除旧 KV 依赖：
  - `CommandType::kSet`
  - `DebugGetValue()`
  - `SetCommand()` / `WaitForValueOnRunningNodes()` / `WaitForValueOnAll()`
- 改为 metadata 写入链路：
  - 先 `CreateBucket`
  - 再对每个对象执行 `CreateObject + CommitObject`
- 新断言：
  - `HeadObject` 可见 committed object
  - `ListObjects` 与预期 object key 集合一致
  - `object_index` / `chunk_ref_index` 可通过 `WaitUntilAllCommittedObject()` 间接验证
  - `request_table` 精确计数
  - `tombstone` 为 0
  - `last_applied_index` 下界推进
  - `last_applied_term > 0`

### 2.2 `test_raft_segment_storage.cpp`

- 删除旧 KV 依赖：
  - `CommandType::kSet`
  - `DebugGetValue()`
  - `SetCommand()` / `WaitForValueOnAll()` / `ProposeWithRetry()`
- 直接存储用例中的伪业务 log payload 从 `SET|...` 改为 metadata 序列化字符串，避免继续扩大 KV command 覆盖面。
- 集群快照/segment 用例改为：
  - `CreateBucket`
  - 批量 `CreateObject + CommitObject`
  - `HeadObject` / `ListObjects`
  - `request_table` / `object_count` / `tombstone_count`
  - `last_applied_index` / `last_applied_term`
- 原有 segment/meta publish、truncate、boundary 断言保持不变，没有改存储业务语义。

### 2.3 `metadata_raft_test_utils.h`

- 新增：
  - `ProposeCreateBucketWithRetry()`
  - `ProposeCreateCommitObjectWithRetry()`
- 目的：
  - 收敛跨文件重复的 metadata proposal 流程
  - 避免在多个 blocker 测试里继续手写 `CreateObject + CommitObject` 重试逻辑

## 3. KV-only 删除候选

- `tests/test_command.cpp`
  - 纯 `CommandType::kSet/kDelete` 编解码单测，属于最终删除窗口的直接退役对象
- `tests/test_state_machine.cpp`
  - 纯 `KvStateMachine` 行为单测，属于最终删除窗口的直接退役对象

## 4. blocker 重扫结果

- 本轮已清掉这两个文件里的 KV blocker：
  - `test_raft_replicator_behavior.cpp`
  - `test_raft_segment_storage.cpp`
- 剩余真实 KV 引用仍在：
  - `tests/test_command.cpp`
  - `tests/test_state_machine.cpp`
  - `tests/test_raft_split_brain.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/test_raft_snapshot_recovery.cpp`
  - `tests/persistence_more_test.cpp`
  - `tests/support/raft_snapshot_restart_test_utils.h`
- 其中 `test_raft_split_brain.cpp` 仍直接实例化 `KvStateMachine`，`raft_snapshot_restart_test_utils.h` 仍封装旧 `Set/Delete + DebugGetValue()` 生命周期，这两处仍是回到删除阶段前的关键 blocker。

## 5. Linux 验证

- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_raft_replicator_behavior test_raft_segment_storage test_raft_log_replication`
  - PASS
- `ctest --test-dir build/linux --output-on-failure -R "(RaftReplicatorBehaviorTest|RaftSegmentStorageTest|RaftLogReplicationTest)"`
  - PASS
  - 23/23 通过

## 6. 日志

- configure：`tmp/test-logs/t038-cmake-configure.log`
- build：`tmp/test-logs/t038-build.log`
- ctest：`tmp/test-logs/t038-ctest.log`
- blocker 重扫：`tmp/test-logs/t038-blocker-rescan.log`

## 7. 结论

- T038 本轮完成了一批风险可控、覆盖价值明确的 blocker 迁移。
- `test_command.cpp` / `test_state_machine.cpp` 已明确归类为 KV-only 删除候选，但本轮未进入最终删除阶段。
- 由于 `split_brain / snapshot_diagnosis / snapshot_recovery / persistence_more / raft_snapshot_restart_test_utils` 仍保留真实 KV 依赖，当前还不能回到删除阶段。
