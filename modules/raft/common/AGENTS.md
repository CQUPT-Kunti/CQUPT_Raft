# Scope

负责共享配置、提案结果和命令编解码，以及 metadata 过渡期入口头文件。
本模块遵守根 AGENTS.md 的 C++ 头文件 / 源文件规则：`.h` 表达接口和契约，`.cpp` 承担具体实现。

## Files

- `command.h`
- `command.cpp`
- `metadata_command.h`
- `metadata_command.cpp`
- `metadata_result.h`
- `config.h`
- `propose.h`

## Responsibilities

- 定义 `Command`
- 定义 metadata command serialization / validation 的共享入口
- 在过渡期通过 `metadata_command.h` 聚合 `raft/metadata` 中的 metadata records / command payload / query model
- 定义 `NodeConfig`、`snapshotConfig`
- 定义 `ProposeResult`
- 保持 `SET|key|value`、`DEL|key|` 命令格式

## Out of Scope

- 不负责选举、复制、持久化、快照调度
- 不负责 RPC 处理

## Dependencies

- 允许依赖：标准库
- 允许依赖：`raft/metadata`
- 不应该依赖：`raft/node`、`raft/service`、`raft/storage`

## Change Rules

- 不要修改命令格式语义
- 不要改公共结构名和字段语义
- 这里只允许做声明、路径和注释层面的维护，除非明确要求改行为

## Relevant Tests

- `tests/test_command.cpp`
- `tests/metadata_command_test.cpp`
- `tests/metadata_manifest_test.cpp`
- `tests/metadata_records_test.cpp`
- `tests/metadata_command_types_test.cpp`
- 大多数 Raft 集成测试会间接使用这里的定义

## Risk Areas

- 命令字符串格式
- metadata 共享结构字段稳定性
- metadata command type / payload 结构稳定性
- 过渡期 umbrella header 与 `raft/metadata` 模块的边界
- 配置字段默认值

## Context Hints

- 修改命令相关内容时先读 `command.h` 和 `command.cpp`
- 修改 metadata 共享结构时先读 `raft/metadata/`，再看 `metadata_command.h`、`metadata_command.cpp`、`metadata_result.h`
- 修改配置相关内容时先读 `config.h`
- 不要为这类变更默认扫描 `raft/node`
