# T009 状态机接口与实现边界

## 1. T009 结论

- T009 已完成，范围仅限 `IStateMachine` 抽象边界整理与实现边界收口。
- 本次没有删除 KV，没有切默认 wiring，没有实现 `MetadataStateMachine` apply/snapshot，也没有拆 proto。
- 当前边界已明确：
  - `RaftNode` 统一通过 `IStateMachine` 持有状态机
  - 统一状态机边界是 `Apply` / `SaveSnapshot` / `LoadSnapshot`
  - `KvStateMachine`、`CompositeKvMetadataStateMachine`、`StrongConsistencyMetadataStateMachine` 都是该接口实现
- `CompositeKvMetadataStateMachine` 仍保留，但已经收缩为 node 模块内部过渡实现，不再暴露在公共头边界上。

## 2. 新增/修改文件

- 新增 `modules/raft/state_machine/state_machine_interface.h`
- 修改 `modules/raft/state_machine/state_machine.h`
- 修改 `modules/raft/state_machine/metadata_state_machine.h`
- 修改 `modules/raft/node/raft_node.h`
- 修改 `modules/raft/node/raft_node.cpp`
- 修改 `modules/raft/state_machine/AGENTS.md`
- 修改 `modules/raft/node/AGENTS.md`
- 新增报告 `specs/006-remove-kv-metadata-state-machine/task-reports/t009-state-machine-boundary.md`

## 3. 边界整理结果

- `state_machine_interface.h`
  - 新承载 `ApplyResult`
  - 新承载 `SnapshotStatus`
  - 新承载 `SnapshotResult`
  - 新承载 `IStateMachine`
- `state_machine.h`
  - 现在只保留 `KvStateMachine` 具体实现定义
  - 通过 include `state_machine_interface.h` 继承统一接口
- `metadata_state_machine.h`
  - 改为直接 include `state_machine_interface.h`
  - 不再通过 `state_machine.h` 间接获得接口
- `raft_node.h`
  - 改为只 include `state_machine_interface.h`
  - 对 `StrongConsistencyMetadataStateMachine` 使用前向声明
  - 不再在公共头中暴露 `KvStateMachine` 或 `CompositeKvMetadataStateMachine`
- `raft_node.cpp`
  - 内部保留 `CompositeKvMetadataStateMachine`
  - 其职责明确为 node 模块内的过渡装配实现，而非最终主路径

## 4. RaftNode 依赖边界

- `RaftNode` 构造函数仍接收 `std::unique_ptr<IStateMachine>`
- `RaftNode::state_machine_` 仍为 `std::unique_ptr<IStateMachine>`
- `ApplyCommittedEntries()` 继续通过 `state_machine_->Apply(...)` 调用统一边界
- snapshot 恢复和保存继续通过：
  - `state_machine_->LoadSnapshot(...)`
  - `state_machine_->SaveSnapshot(...)`
- 结论：`RaftNode` 的持有与调用边界已经明确落在 `IStateMachine`

## 5. CMake 更新

- 本次未修改根 `CMakeLists.txt`
- 本次未修改 `tests/CMakeLists.txt`
- 原因：新增的是头文件边界拆分，不涉及新的 source target、link 关系或测试 target 拆分

## 6. AGENTS.md 更新

- `modules/raft/state_machine/AGENTS.md`
  - 补充 `state_machine_interface.h`
  - 明确统一状态机抽象边界职责
  - 明确该模块不决定默认业务主路径
  - 标记 `CompositeKvMetadataStateMachine` 与统一接口的过渡关系
- `modules/raft/node/AGENTS.md`
  - 增加 `IStateMachine` 抽象边界与过渡期 composite 装配风险说明
  - 明确 `CompositeKvMetadataStateMachine` 只应留在 node 内部，不应扩散到公共头边界
- AGENTS 更新只涉及模块结构和职责说明，没有写执行日志

## 7. 测试结果

- Linux configure：
  - 命令：`cmake --preset debug-ninja-low-parallel`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t009-configure.log`
- Linux build：
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target test_state_machine test_metadata_state_machine`
  - 结果：`PASS`
  - 覆盖：重编了 `raft_node.cpp`、`state_machine.cpp`、`metadata_state_machine.cpp` 及相关 `raft_core` 依赖
  - 日志：`tmp/test-logs/t009-build.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^(KvStateMachineTest|MetadataStateMachineTest)\\."`
  - 结果：`PASS`
  - 统计：`13/13` 通过
  - 日志：`tmp/test-logs/t009-ctest.log`
- 本任务只运行了 T009 相关个别测试，未运行全量 CTest。

## 8. 未执行测试说明

- 未运行 `raft_integration_test`、`snapshot_test`、`persistence_test`、`test_raft_snapshot_restart` 等更大范围回归；原因是 T009 只整理抽象边界，不改业务语义
- Windows configure/build/CTest 未执行；原因与前序任务一致，当前环境为 Linux，`windows` preset 依赖 `Visual Studio 17 2022` generator
- 未运行 `MetadataService`、proto、client 相关测试；这些不属于 T009 范围

## 9. 风险点

- `CompositeKvMetadataStateMachine` 仍存在，因此当前仓库仍处于 KV/metadata 双栈过渡态
- 虽然 `RaftNode` 头文件边界已收紧，但 `raft_node.cpp` 仍直接依赖 `KvStateMachine` 与 `StrongConsistencyMetadataStateMachine`
- `DebugGetValue()`、`Describe()` 等 KV-oriented 调试路径尚未移除，仍属于后续任务范围
- 当前只是抽象边界清理，不代表默认主路径已变为 metadata-only

## 10. 验收结果

- `RaftNode` 只通过 `IStateMachine` 持有和调用状态机边界这一点已明确
- `Apply` / `SaveSnapshot` / `LoadSnapshot` 已被抽到独立接口头
- `KvStateMachine`、`CompositeKvMetadataStateMachine`、未来 `MetadataStateMachine` 的统一接口边界已明确
- `IStateMachine` 不依赖 KV，也不依赖 metadata
- `CompositeKvMetadataStateMachine` 保留为过渡实现，并已收回到 node 内部实现细节
- 未删除 KV
- 未修改默认 wiring
- 未实现 `MetadataStateMachine` apply / snapshot
- 未拆 proto
- 未进入 T010 或后续任务
