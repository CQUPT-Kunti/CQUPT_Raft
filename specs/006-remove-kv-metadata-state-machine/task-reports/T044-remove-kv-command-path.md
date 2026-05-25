# T044 删除 common Command 中的 KV SET/DEL 命令路径

## 结果
- 本任务当前判定为 **blocked**。
- 本轮未执行 `modules/raft/common/command.h` / `command.cpp` 中 `SET/DEL` 的物理删除。
- 原因：精确扫描确认仍存在多个未迁移 blocker，当前在用户限定可修改范围内无法安全完成删除。

## 已执行的精确扫描
- 关键词：
  - `CommandType::kSet`
  - `CommandType::kDelete`
  - `SetCommand`
  - `DeleteCommand`
  - `DebugGetValue`
- 扫描结论：
  - `common Command` 自身仍定义并解析 `SET|key|value` / `DEL|key|`
  - 高价值 recovery/snapshot/catch-up 测试仍大量直接依赖旧 KV helper/断言
  - 生产代码仍直接引用 `CommandType::kSet/kDelete`

## blocker 详情
- 生产代码 blocker：
  - `modules/raft/node/raft_node.cpp`
    - 仍直接判断 `command.type == CommandType::kSet || kDelete`
    - 仍使用 `command.key` / `command.value` 做校验
  - `modules/raft/state_machine/state_machine.cpp`
    - 仍直接 apply `CommandType::kSet/kDelete`
- 测试 blocker：
  - `tests/support/raft_snapshot_restart_test_utils.h`
    - 仍定义 `SetCommand` / `DeleteCommand`
    - 仍使用 `DebugGetValue`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/test_raft_snapshot_recovery.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/persistence_test.cpp`
  - `tests/snapshot_test.cpp`
  - `tests/test_state_machine.cpp`
  - `tests/test_raft_election.cpp`
- 其中高价值 blocker：
  - `persistence_test.cpp`
  - `snapshot_test.cpp`
  - `test_raft_snapshot_catchup.cpp`
  - `test_raft_snapshot_restart.cpp`
  - `test_raft_snapshot_recovery.cpp`
  - `tests/support/raft_snapshot_restart_test_utils.h`

## 为什么本轮不能安全删除
- 若直接删除 `CommandType::kSet/kDelete`：
  - `raft_node.cpp` 无法编译
  - `state_machine.cpp` 无法编译
  - 多个恢复/快照/追赶测试会在编译期或运行期直接断裂
- 用户本轮允许修改的文件列表不包含上述生产 blocker 文件：
  - `modules/raft/node/raft_node.cpp`
  - `modules/raft/state_machine/state_machine.cpp`
- 因此本轮不能在不扩大范围的前提下完成真实删除。

## 删除了哪些 KV command 符号
- 本轮未删除。

## 哪些旧引用被迁移到 MetadataCommand
- 本轮未迁移。
- 阻塞判断完成后停止扩大修改，避免在未获授权范围内继续改生产/测试链路。

## 抽取或复用了哪些测试 helper
- 本轮未新增或修改 helper。

## 是否仍有 KV command blocker
- 有，且 blocker 仍覆盖生产代码与高价值恢复测试。

## Linux 验证命令和结果
- 执行了精确扫描：
  - `rg -n "CommandType::kSet|CommandType::kDelete|\\bSetCommand\\b|\\bDeleteCommand\\b|DebugGetValue\\(" modules tests`
- 结果：
  - FAIL，前置门禁未通过，不适合继续做 `configure/build/ctest`
- 说明：
  - 因未执行公共接口删除，所以未运行建议中的 `cmake --build ...` / `ctest ...`
  - 当前继续构建验证没有意义，因为删除动作本身尚未具备安全前提

## 是否可以进入 T045
- 不可以。
- 需要先完成至少以下 blocker 迁移后，才能重新执行 T044：
  - `tests/support/raft_snapshot_restart_test_utils.h`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/test_raft_snapshot_recovery.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/persistence_test.cpp`
  - `tests/snapshot_test.cpp`
  - 以及与 `CommandType::kSet/kDelete` 直连的生产残留：
    - `modules/raft/node/raft_node.cpp`
    - `modules/raft/state_machine/state_machine.cpp`
