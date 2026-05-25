# T037 删除旧 KV 状态机 / KV Command 残留

## 1. 门禁结论

- 本轮先做了旧 KV 残留的精确引用门禁扫描，没有直接进入删除。
- 结论：`KvStateMachine`、`CommandType::kSet/kDelete`、`DebugGetValue()`、`CompositeKvMetadataStateMachine` 仍有真实源码引用和真实测试引用，当前不能安全删除。
- 因此本轮没有强删 KV 核心代码，也没有修改 `RaftNode` 默认 wiring；T037 当前停在 blocker 归档阶段，不能宣称完成 KV residue 删除。

## 2. 扫描到的核心 KV 残留

### 2.1 源码侧活跃引用

- `modules/raft/common/command.h/.cpp`
  - `CommandType::kSet`
  - `CommandType::kDelete`
  - `SET|key|value` / `DEL|key|` 编解码
- `modules/raft/state_machine/state_machine.h/.cpp`
  - `KvStateMachine`
  - KV snapshot save/load
- `modules/raft/node/raft_node.cpp`
  - `CompositeKvMetadataStateMachine`
  - `KvStateMachine kv_`
  - `RaftNode::DebugGetValue()`
  - `ValidateCommandUnlocked()` 中的 `kSet/kDelete` 校验分支
  - `GetMetadataStateMachine*()` 中对 composite 的 `dynamic_cast`

### 2.2 测试侧真实依赖

- KV-only 单测，删除前必须一并清理或替换：
  - `tests/test_command.cpp`
  - `tests/test_state_machine.cpp`
- 仍借用 KV 写入/断言的 Raft 高价值回归：
  - `tests/test_raft_election.cpp`
  - `tests/test_raft_split_brain.cpp`
  - `tests/test_raft_replicator_behavior.cpp`
  - `tests/test_raft_segment_storage.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/test_raft_snapshot_recovery.cpp`
  - `tests/persistence_more_test.cpp`
  - `tests/persistence_test.cpp` 仍有未迁移 KV restart boundary 用例
  - `tests/snapshot_test.cpp` 仍有未迁移 KV snapshot 用例
  - `tests/test_raft_snapshot_catchup.cpp` 仍有未迁移 KV catch-up 用例
  - `tests/support/raft_snapshot_restart_test_utils.h`

## 3. blocker 分类

### 3.1 可直接删除但当前不能单独删的对象

- `test_command.cpp` / `test_state_machine.cpp` 本身是 KV-only。
- 但它们依赖 `CommandType::kSet/kDelete` 与 `KvStateMachine`，如果先删源码再删测试，会先把 `tests/CMakeLists.txt` 和现有 unit target 打断。

### 3.2 必须先迁移、不能硬删的对象

- `RaftNode::DebugGetValue()`：
  - 仍被多组 persistence / snapshot / split-brain / diagnosis / replicator / segment-storage 回归直接用作断言入口。
- `CompositeKvMetadataStateMachine`：
  - 当前仍是 `RaftNode` 内兼容旧 KV 测试的桥接实现，删掉会直接影响 `GetMetadataStateMachine*()` 的兼容分支和旧测试运行。
- `CommandType::kSet/kDelete`：
  - 仍被旧测试 payload 构造与命令编解码测试使用，不能在同一轮只删 enum/codec 不迁移测试。

### 3.3 迁移复杂度最高的 blocker

- `tests/test_raft_split_brain.cpp`
  - 既依赖 `DebugGetValue()`，又直接实例化 `KvStateMachine snapshot_state`，不是简单替换断言即可完成。
- `tests/support/raft_snapshot_restart_test_utils.h`
  - 仍封装旧 `Set/Delete + DebugGetValue()` 生命周期，多个 snapshot/restart 测试共用，需先整体迁移 helper。

## 4. 本轮删除/迁移结果

- 删除文件：无。
- 删除符号：无。
- 删除 CMake 项：无。
- 迁移引用：无。
- 原因：门禁确认仍有真实引用，按任务约束不得强删导致测试体系断裂。

## 5. Linux 验证

- 本轮未修改业务源码、测试源码或 CMake；未触发 configure/build/ctest。
- 执行的 Linux 门禁验证命令：
  - `rg -n --hidden --glob '!build/**' --glob '!.git/**' --glob '!tmp/**' --glob '!deploy/**' --glob '!vcpkg_installed/**' --glob '!**/.gitignore' "KVCommand|KvStateMachine|KVStateMachine|CommandType::kSet|CommandType::kDelete|DebugGetValue|DebugPut|DebugDelete|kv_state_machine|\\bkv=\\{|\\bPut\\(|\\bGet\\(|\\bDelete\\(" modules tests apps proto CMakeLists.txt tests/CMakeLists.txt`
- 结果：PASS，成功确认核心 KV residue 仍有活跃引用。
- 完整扫描日志：`tmp/test-logs/t037-kv-gate-scan.log`

## 6. 当前 blocker 与后续建议

- blocker 仍存在，当前不能删除：
  - `modules/raft/common/command.*`
  - `modules/raft/state_machine/state_machine.*`
  - `modules/raft/node/raft_node.cpp` 中 KV/composite/debug 分支
- 后续若继续推进删除，需要先新增专门迁移任务，至少覆盖：
  - `test_command.cpp` / `test_state_machine.cpp` 的退场方案
  - `test_raft_split_brain.cpp`
  - `test_raft_replicator_behavior.cpp`
  - `test_raft_segment_storage.cpp`
  - `test_raft_snapshot_diagnosis.cpp`
  - `test_raft_snapshot_recovery.cpp`
  - `persistence_more_test.cpp`
  - `raft_snapshot_restart_test_utils.h` 及其调用方

## 7. 验收结论

- T037 本轮完成了“删除前置引用门禁”。
- T037 本轮未满足“删除旧 KV 状态机 / KV Command 残留”的最终验收，因为 blocker 尚未清空。
- 当前不能进入 T038；必须先补一轮剩余 KV 依赖迁移，再回到删除阶段。
