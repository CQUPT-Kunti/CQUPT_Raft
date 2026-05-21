# Scope

负责 KV / metadata 状态机与状态机快照格式。

本模块遵守根 AGENTS.md 的 C++ 头文件 / 源文件规则：`.h` 表达接口和契约，`.cpp` 承担具体实现。

## Files

- `state_machine_interface.h`
- `state_machine.h`
- `state_machine.cpp`
- `metadata_state_machine.h`
- `metadata_state_machine.cpp`

## Responsibilities

- 定义统一的 `IStateMachine` / `Apply` / `SaveSnapshot` / `LoadSnapshot` 抽象边界
- 应用 `SET` / `DEL` 命令
- 提供 `MetadataStateMachine` V2 骨架占位
- `MetadataStateMachine` 当前可承载 bucket 级最小 apply 占位
- 应用 metadata-only 状态机查询与生命周期逻辑
- 提供 `Get` 与 `DebugString`
- 保存和加载状态机 snapshot

## Out of Scope

- 不负责 quorum、term、vote、leader
- 不负责 snapshot catalog 管理
- 不负责 gRPC 适配
- 不决定默认业务主路径选择

## Dependencies

- 允许依赖：`raft/common`
- 不应该依赖：`raft/service`、`apps`

## Change Rules

- 不要改命令语义
- 不要改状态机 snapshot 二进制格式
- 路径调整之外，不要在这里补 Raft 逻辑
- `MetadataStateMachine` 骨架可以返回明确的未实现错误，但不能静默 no-op 后返回成功
- 在未明确任务前，不要把 `MetadataStateMachine` 的 object/apply/snapshot 占位扩展成完整业务实现
- `StrongConsistencyMetadataStateMachine` 仍代表现有 metadata V1 行为，不要在未明确任务时替换默认 wiring

## Relevant Tests

- `tests/test_state_machine.cpp`
- `tests/metadata_state_machine_test.cpp`
- `tests/metadata_snapshot_test.cpp`
- `tests/test_raft_commit_apply.cpp`
- `tests/snapshot_test.cpp`
- `tests/test_raft_snapshot_restart.cpp`

## Risk Areas

- snapshot 文件头
- key/value 序列化顺序
- metadata state machine 的查询/快照边界
- 过渡期 `CompositeKvMetadataStateMachine` 与统一接口的关系
- noop 命令处理

## Context Hints

- 先读 `state_machine_interface.h`
- 先读 `state_machine.h`
- metadata-only 路径先读 `metadata_state_machine.h`
- 再读 `Apply`、`SaveSnapshot`、`LoadSnapshot`
- 不需要时不要默认进入 `raft/node`
