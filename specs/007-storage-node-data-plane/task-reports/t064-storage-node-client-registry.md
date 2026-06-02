# T064 Storage Node Client Registry

## 修改文件

- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/module-notes.md`
- `modules/store/node/AGENTS.md`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t064-storage-node-client-registry.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `StorageNodeClient` 中补齐以下同步 gRPC 调用：
  - `RegisterStorageNode`
  - `UpdateStorageNodeHeartbeat`
  - `ReportHealth`
  - `ReportCapacity`
  - `ReportLoad`
- 新增 client 侧本地请求/响应结构，表达：
  - register 的 `created` / `idempotent` / snapshot
  - heartbeat/report 的 `accepted_sequence` / `applied` / `idempotent` / `stale_ignored` / snapshot
- 在 client 侧补齐：
  - 本地 request -> proto request 转换
  - proto response / gRPC status -> 本地 response 转换
  - `StorageNodeRegistrySnapshot`、health/capacity/load/facts/liveness 的反序列化
  - `timeout_ms -> grpc::ClientContext` deadline 映射
- 扩展 `storage_node_client_test`，同时覆盖：
  - fake stub 下的 request 字段映射
  - gRPC `DEADLINE_EXCEEDED` / `CANCELLED` / `UNAVAILABLE` / `INVALID_ARGUMENT` / `INTERNAL` 映射
  - 真实 `StorageNodeService + StorageNodeRegistry` 链路下的 duplicate register、endpoint conflict、stale heartbeat、same-sequence idempotent、partial report merge

## StorageNodeClient register/heartbeat/report 字段映射和状态语义

- `RegisterStorageNode`
  - 本地 `request_id`、`node_id`、`endpoint`、`observed_at_unix_ms`、`facts` 映射到 proto `RegisterStorageNodeRequest`
  - proto `summary.code/message/retry_after_ms`、`created`、`idempotent`、`snapshot` 映射回本地 `StorageNodeClientRegisterStorageNodeResponse`
- `UpdateStorageNodeHeartbeat`
  - 本地 `request_id`、`node_id`、`endpoint`、`sequence`、`observed_at_unix_ms`、全量 `facts` 映射到 proto `UpdateStorageNodeHeartbeatRequest.heartbeat`
  - proto `accepted_sequence`、`applied`、`idempotent`、`stale_ignored`、`snapshot` 映射回本地 `StorageNodeClientFactUpdateResponse`
- `ReportHealth` / `ReportCapacity` / `ReportLoad`
  - 本地 identity、`sequence`、`observed_at_unix_ms` 和局部 facts 映射到对应 proto report request
  - partial merge 语义完全复用 T062/T063 的 registry + service contract
- 状态语义
  - duplicate register 同 endpoint 仍为 idempotent
  - duplicate register 不同 endpoint 仍返回 conflict
  - stale heartbeat 仍返回 `already_exists + stale_ignored`
  - same sequence heartbeat 仍返回 `ok + idempotent`
  - 所有这些语义都由 service / registry 决定，client 只做映射

## deadline / cancellation 当前边界

- heartbeat/register/report client 现在会把本地 `timeout_ms` 设置到 `grpc::ClientContext` deadline
- gRPC `DEADLINE_EXCEEDED` 映射为 `kTimeout`
- gRPC `CANCELLED` 映射为 `kCancelled`
- gRPC `UNAVAILABLE` 映射为 `kNodeUnavailable`
- 其他非 OK gRPC status 会映射成明确错误，不返回 silent success
- 当前 `proto/storage_node.proto` 的 register/heartbeat/report 请求没有 `timeout_ms` / `best_effort_cancel` 字段
- 因此 `best_effort_cancel` 目前只能保留为本地边界说明，不能伪装成已经透传到 service / registry / runtime
- 当前没有为这些 control-plane RPC 追加自动重试策略；调用方需根据映射后的状态自行决定是否重试

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不保存 payload
- client 只发起 StorageNode data-plane control-plane RPC

## 是否使用 tests/test_file/test_file.zip

- 否

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_client|storage_node_service|storage_heartbeat_registry" --output-on-failure 2>&1 | tee tmp/007/t064-storage-node-client-registry.log`
  - PASS
  - 实际匹配到的测试名为 `storage_node_service`、`storage_node_client`、`storage_heartbeat_registry`
  - 日志路径：`tmp/007/t064-storage-node-client-registry.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T064 是 client adapter 和平台无关测试，一般不单列 `T064-WIN`
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未引入新的平台相关文件 IO、网络协议分支或时间源实现

## 是否通过 T064

- 是

## 是否可以进入 T065

- 可以
- 前提：T065 只消费当前已经固定的 registry facts / snapshot / stale-idempotent contract，不扩展 proto 或重写 registry/service/client 语义

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，执行 `007` 任务时需要人工纠偏
- T061 既有 proto 没有 register/heartbeat/report 的 `timeout_ms` / `best_effort_cancel` 字段，因此 T064 无法提供真正的 on-wire cancel hint，只能用 gRPC deadline 表达超时边界
- 当前 control-plane client 不自动重试 timeout / unavailable；后续如果需要统一 retry budget，应单独收口

## 是否更新 module-notes.md / AGENTS.md

- 是
- `modules/store/node/module-notes.md`：补充 client control-plane 职责、helper 和 deadline/cancellation 边界
- `modules/store/node/AGENTS.md`：更新 heartbeat/report/register client 已实现的模块边界

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - register request/response 转换 helper
  - heartbeat request/response 转换 helper
  - health/capacity/load report 转换 helper
  - gRPC status 映射 helper
  - deadline helper
  - `StorageNodeClient::RegisterStorageNode` 流程
  - `StorageNodeClient::UpdateStorageNodeHeartbeat` 流程
  - `StorageNodeClient::ReportHealth / ReportCapacity / ReportLoad` 流程

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T064 标记完成，并记录实际影响文件
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：收缩“client 未接线”风险，并补充 control-plane deadline / cancel / retry 边界

## common-risk-notes.md 读取结果

- 已读取并维护
- 原有 Windows durability、GC/recovery、Placement/read-side 接线、clock/sequence freshness 风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T064`，记录 control-plane RPC 当前只有 gRPC deadline、没有 on-wire cancel hint，也没有自动重试策略
- 删除：无整项删除
- 收缩：
  - `T059` 从“client/consumer 未接线”收缩为“consumer 未接线”
  - `T061` 从“client/consumer 与 fake stub 演进风险”收缩为“consumer 与 fake stub 演进风险”
  - `T062` 从“service/client 未接线”收缩为“placement/read-side 未接线”
  - `T063` 从“client 未收口”收缩为“service 仍信任调用方时间源/sequence”
