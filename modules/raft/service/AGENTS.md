# Scope

负责 gRPC Raft/KV/Metadata 服务适配层。

## Files

- `raft_service_impl.h`
- `raft_service_impl.cpp`
- `kv_service_impl.h`
- `kv_service_impl.cpp`
- `metadata_service_impl.h`
- `metadata_service_impl.cpp`

## Responsibilities

- 接收 gRPC 回调
- 调用 `RaftNode`
- 回填 protobuf 响应
- 记录 RPC 延迟指标
- 维护按 proto 文件拆分后的 include 边界

## Out of Scope

- 不拥有 Raft 共识状态
- 不定义 protobuf schema
- 不定义持久化行为

## Dependencies

- 允许依赖：`proto`、`raft/node`、`raft/common`
- 不应该依赖：`apps`

## Change Rules

- 不要改变 RPC 语义
- 不要改状态码含义
- 这里只改适配层和路径，不把业务逻辑塞进 service 层
- `raft_service_impl` 只应依赖 `raft.proto` 生成头
- `metadata_service_impl` 只应依赖 `metadata.proto` 生成头
- `kv_service_impl` 只应依赖 `kv.proto` / `common.proto` 生成头

## Relevant Tests

- `tests/test_kv_service.cpp`
- `tests/metadata_failover_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `tests/raft_integration_test.cpp`

## Risk Areas

- leader redirect 语义
- status/metrics 字段填充
- 跨 proto import 后的生成头对齐
- 过渡期遗留 KV RPC 与 metadata RPC 的边界清晰度

## Context Hints

- 先读对应 proto：`raft.proto` / `metadata.proto` / `kv.proto`
- 再读对应 service 实现
- 需要行为确认时再进入 `raft/node`
