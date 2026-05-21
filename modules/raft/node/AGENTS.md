# Scope

负责 `RaftNode` 核心状态、选举、提交、应用、恢复与快照调度。
本模块遵守根 AGENTS.md 的 C++ 头文件 / 源文件规则：`.h` 表达接口和契约，`.cpp` 承担具体实现。

## Files

- `raft_node.h`
- `raft_node.cpp`

## Responsibilities

- 维护 role、term、vote、leader、log、commit/apply 状态
- 启停 gRPC server、scheduler、snapshot worker
- 处理 `RequestVote`、`AppendEntries`、`InstallSnapshot`
- 驱动复制、提交推进、状态机应用
- 启动恢复和 snapshot 调度

## Out of Scope

- 不负责 protobuf schema 定义
- 不负责底层存储文件格式实现细节
- 不负责 CLI 行为

## Dependencies

- 允许依赖：`raft/common`、`raft/runtime`、`raft/service`、`raft/replication`、`raft/storage`、`raft/state_machine`
- 不应该依赖：`apps`

## Change Rules

- 这是高风险模块
- 不要改选举、复制、apply、snapshot、recovery 语义，除非任务明确要求
- 做结构调整时必须同步检查相关测试

## Relevant Tests

- `tests/test_raft_election.cpp`
- `tests/test_raft_log_replication.cpp`
- `tests/test_raft_commit_apply.cpp`
- `tests/test_raft_split_brain.cpp`
- `tests/persistence_test.cpp`
- `tests/raft_integration_test.cpp`
- `tests/snapshot_test.cpp`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_snapshot_restart.cpp`
- `tests/test_raft_snapshot_diagnosis.cpp`

## Risk Areas

- term/role 切换
- commit/apply 顺序
- snapshot 裁剪边界
- startup replay
- `IStateMachine` 抽象边界与过渡期 composite 装配
- 并发锁顺序

## Context Hints

- 先读 `raft_node.h`
- 关注状态机装配时，先确认这里只依赖 `IStateMachine` 抽象，再看具体实现类型
- 过渡期 `CompositeKvMetadataStateMachine` 仅应作为 node 内部实现细节存在，不应扩散到公共头边界
- 再读 `raft_node.cpp` 中目标函数附近
- 需要复制细节时再读 `raft/replication`
- 需要持久化边界时再读 `raft/storage`
