# Scope

负责共享配置、提案结果和命令编解码。
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
- 定义 metadata records / metadata command / metadata result 的共享契约
- 定义 `NodeConfig`、`snapshotConfig`
- 定义 `ProposeResult`
- 保持 `SET|key|value`、`DEL|key|` 命令格式

## Out of Scope

- 不负责选举、复制、持久化、快照调度
- 不负责 RPC 处理

## Dependencies

- 允许依赖：标准库
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
- 大多数 Raft 集成测试会间接使用这里的定义

## Risk Areas

- 命令字符串格式
- metadata 共享结构字段稳定性
- 配置字段默认值

## Context Hints

- 修改命令相关内容时先读 `command.h` 和 `command.cpp`
- 修改 metadata 共享结构时先读 `metadata_command.h`、`metadata_command.cpp`、`metadata_result.h`
- 修改配置相关内容时先读 `config.h`
- 不要为这类变更默认扫描 `raft/node`
