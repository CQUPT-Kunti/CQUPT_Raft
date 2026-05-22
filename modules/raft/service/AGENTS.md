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
- 维护与 proto target 对应的最小链接边界
- `MetadataService` 写路径必须封装 `MetadataCommand` 并通过 `RaftNode::ProposeMetadata(...)` 进入 Raft
- `MetadataService` 读路径只允许走 `MetadataStateMachine` 本地查询，不把 `Head/List` 伪装成写日志命令
- `MetadataService` 不允许回退到 `KvService` / KV message / `KvStateMachine`

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
- `raft_core` 内的 service 实现应最小化消费：
  - `raft_service_impl` -> `raft_proto`
  - `metadata_service_impl` -> `metadata_proto`
  - `kv_service_impl` -> `kv_proto`
- `metadata_service_impl` 的 leader redirect / timeout / conflict / not-found 语义必须通过 metadata 状态码显式回传
- `metadata_service_impl` 可以保留 leader 本地读，但如果没有 `ReadIndex` / leader lease，必须把“非严格线性一致读”的风险写入报告

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
- 如果问题出在链接面或生成头暴露面，同时检查根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`
