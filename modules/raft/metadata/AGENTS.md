# Scope

负责 metadata-only 业务模型的共享类型边界，不负责共识流程本身。

## Files

- `metadata_records.h`
- `metadata_command_types.h`
- `metadata_query.h`

## Responsibilities

- 定义 bucket/object/chunk/request records
- 定义 metadata write command payload skeleton
- 定义 head/list query model
- 为后续 `MetadataStateMachine`、`MetadataService`、metadata command serialization 提供清晰 include 边界

## Out of Scope

- 不负责 `RaftNode` 装配
- 不负责 gRPC/proto
- 不负责 snapshot 持久化实现
- 不负责命令序列化逻辑

## Dependencies

- 允许依赖：标准库
- 不应该依赖：`raft/node`、`raft/service`、`raft/storage`、KV 模块

## Change Rules

- 这里只维护类型、结构体、枚举、轻量 inline helper
- 不要把状态机 apply、序列化、文件 IO、RPC 逻辑写进本模块
- 结构拆分可以做，业务语义扩展留给后续任务

## Relevant Tests

- `tests/metadata_records_test.cpp`
- `tests/metadata_command_types_test.cpp`
- `tests/metadata_command_test.cpp`
- `tests/metadata_manifest_test.cpp`

## Risk Areas

- records / command payload / query model 的边界是否清晰
- include 方向是否形成循环依赖
- 过渡期 `raft/common/metadata_command.h` 与本模块的职责重叠

## Context Hints

- 先读 `metadata_records.h`
- 再读 `metadata_command_types.h`
- 查询模型问题读 `metadata_query.h`
- 不需要时不要默认进入 `raft/node` 或 `raft/service`
