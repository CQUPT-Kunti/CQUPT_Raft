## T045 结果

- 当前任务判定为 `blocked`，本次未删除 `modules/raft/state_machine/state_machine.h/.cpp`，也未退役 `tests/test_state_machine.cpp`。
- 原因：删除前门禁扫描发现生产代码和多处高价值回归测试仍存在真实 KV 状态机依赖；按任务要求不能硬删。

## 是否删除 state_machine.h / state_machine.cpp

- 否。
- 阻塞点：
  - [raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:24) 仍直接 `#include "raft/state_machine/state_machine.h"`。
  - [raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:29) 仍保留内部 `CompositeKvMetadataStateMachine`。
  - [raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:626) / [raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:643) 仍 `dynamic_cast<KvStateMachine>` 并暴露 `DebugGetValue()` / `Describe()` 的 KV 调试支路。
  - [CMakeLists.txt](/home/yangjilei/Code/C++/CQUPT_Raft/CMakeLists.txt:219) 仍把 `modules/raft/state_machine/state_machine.cpp` 编进 `raft_core`。

## 是否退役 test_state_machine.cpp

- 否。
- [tests/test_state_machine.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_state_machine.cpp:1) 确实是纯 `KvStateMachine` 单测。
- 但在生产代码和高价值回归仍依赖 KV 状态机语义之前，直接移除该测试会让仍在仓内的 KV 支路失去基础覆盖。
- [tests/CMakeLists.txt](/home/yangjilei/Code/C++/CQUPT_Raft/tests/CMakeLists.txt:227) 目前仍注册 `test_state_machine` target。

## 真实引用与未迁移项

- 生产代码：
  - `modules/raft/node/raft_node.cpp` 仍依赖 `KvStateMachine` / `CompositeKvMetadataStateMachine`。
  - `modules/raft/common/command.cpp` 仍保留 `CommandType::kSet` / `CommandType::kDelete` 编解码分支。
- 高价值回归测试：
  - [tests/test_raft_snapshot_catchup.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_snapshot_catchup.cpp:59) 仍定义 `SetCommand` / `DeleteCommand`，并在多处使用 `DebugGetValue()`。
  - [tests/test_raft_snapshot_recovery.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_snapshot_recovery.cpp:28) 仍通过 `SetCommand` / `DeleteCommand` 和 `DebugGetValue()` 验证恢复结果。
  - [tests/test_raft_snapshot_restart.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_snapshot_restart.cpp:19) 仍依赖 `SetCommand`。
  - [tests/snapshot_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/snapshot_test.cpp:346) 仍使用 `DebugGetValue()`，且 [snapshot_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/snapshot_test.cpp:543) 仍构造 `CommandType::kSet`。
  - [tests/persistence_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/persistence_test.cpp:248) 起仍大量构造 `CommandType::kSet` 并使用 `DebugGetValue()`。
  - [tests/raft_integration_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/raft_integration_test.cpp:483) 仍有 `ReplicatesSetAndDeleteCommandsToAllNodes`。
  - [tests/test_raft_commit_apply.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_commit_apply.cpp:293) 仍有 `DeleteCommandIsAppliedToAllNodes`。
  - [tests/support/raft_snapshot_restart_test_utils.h](/home/yangjilei/Code/C++/CQUPT_Raft/tests/support/raft_snapshot_restart_test_utils.h:103) 仍提供 `SetCommand` / `DeleteCommand` helper 与 `DebugGetValue()` 断言辅助。

## 删除/迁移内容

- 本次未做源码删除或迁移。
- 仅新增 blocker 报告，避免在未迁移高价值测试前强行删除旧 `KvStateMachine` 路径。

## Linux 验证

- 未执行 `cmake` / `build` / `ctest`。
- 原因：本任务在删除前门禁阶段即判定 `blocked`，继续强删会引入已知断裂，不符合任务要求。

## 是否仍有 blocker

- 有。
- 主要 blocker 是：
  - `RaftNode` 内部仍保留 `KvStateMachine` / `CompositeKvMetadataStateMachine` 调试与兼容分支。
  - 多个恢复 / snapshot / catch-up / integration 高价值回归仍依赖 `SetCommand` / `DeleteCommand` / `DebugGetValue()`。
  - `CommandType::kSet` / `kDelete` 公共路径仍存在。

## 是否可以进入后续删除任务

- 当前不建议进入后续删除任务。
- 需要先完成这些真实 KV 引用的迁移或清理，再回到 T045 执行物理删除。
