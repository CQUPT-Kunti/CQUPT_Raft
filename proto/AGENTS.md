# Scope

负责 RPC/Protobuf 契约层。

## Files

- `raft.proto`
- `metadata.proto`
- `common.proto`
- `kv.proto`（过渡期保留，用于承载尚未删除的 KV RPC）

## Responsibilities

- 定义 `RaftService`
- 定义 `MetadataService`
- 定义共享 request/response/message/enum
- 仅在过渡期于 `kv.proto` 中定义 `KvService`

## Out of Scope

- 不负责业务实现
- 不负责持久化格式
- 不负责测试调度

## Dependencies

- 允许依赖：无源码级依赖
- 不应该依赖：任何 C++ 模块

## Change Rules

- 高风险区域
- 不要修改协议语义，除非任务明确要求并同步更新所有调用方与测试
- 字段编号、消息名、状态码都视为稳定契约
- 优先维持文件边界清晰：
  - `raft.proto` 只承载共识 RPC 与 Raft 消息
  - `metadata.proto` 只承载 metadata 业务 RPC
  - `common.proto` 只承载可复用公共消息
  - `kv.proto` 只承载遗留 KV RPC，避免重新混回 `raft.proto`

## Relevant Tests

- `tests/test_kv_service.cpp`
- `tests/metadata_failover_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `tests/raft_integration_test.cpp`
- 其余绝大部分集群测试都会间接依赖这里

## Risk Areas

- message 字段编号
- 状态码枚举
- 跨 proto import 与生成顺序
- Raft RPC、metadata RPC、遗留 KV RPC 的边界串扰

## Context Hints

- 修改前先确认是不是必须动 `proto`
- 若只是实现层问题，不要进入该目录
- 修改后同步检查根 `CMakeLists.txt` 的 proto generation target 和直接 include 生成头的 service/client/test
