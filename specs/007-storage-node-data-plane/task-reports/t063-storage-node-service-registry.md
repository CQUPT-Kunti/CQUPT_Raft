# T063 Storage Node Service Registry

## 修改文件

- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/module-notes.md`
- `modules/store/node/AGENTS.md`
- `tests/storage_node_service_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t063-storage-node-service-registry.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `StorageNodeService` 中实现以下 gRPC 入口：
  - `RegisterStorageNode`
  - `UpdateStorageNodeHeartbeat`
  - `ReportHealth`
  - `ReportCapacity`
  - `ReportLoad`
- 为 `StorageNodeService` 增加可选注入的 `StorageNodeRegistry`，并在 service 层完成：
  - proto request -> 本地 registry request 转换
  - registry result -> proto response 转换
  - facts / snapshot / liveness / accepted_sequence / stale_ignored / idempotent 字段映射
- 扩展 `storage_node_service_test`，覆盖 register、duplicate register、heartbeat apply/stale/idempotent、partial report merge、invalid request 和 service 不走 chunk/metadata/Raft 路径的边界。

## StorageNodeService register/heartbeat/report 字段映射和状态语义

- `RegisterStorageNode`
  - proto `request_id` 只用于 response summary，不进入 registry
  - `node_id`、`endpoint`、`observed_at_unix_ms`、`facts` 转给 `StorageNodeRegistry::RegisterStorageNode`
  - response 返回 `created`、`idempotent` 和 registry snapshot
- `UpdateStorageNodeHeartbeat`
  - 读取 `request.heartbeat` 中的 `node_id`、`endpoint`、`sequence`、`observed_at_unix_ms`、`facts`
  - 转给 `StorageNodeRegistry::UpdateStorageNodeHeartbeat`
  - response 返回 `accepted_sequence`、`applied`、`idempotent`、`stale_ignored` 和 snapshot
- `ReportHealth` / `ReportCapacity` / `ReportLoad`
  - 读取 proto 中的 identity、`sequence`、`observed_at_unix_ms` 与对应局部 facts
  - 分别转给 `StorageNodeRegistry::ReportHealth` / `ReportCapacity` / `ReportLoad`
  - response 统一复用 `StorageNodeFactUpdateResponse`
- 状态语义
  - `same sequence`、`stale heartbeat`、`duplicate register`、`endpoint conflict` 完全复用 T062 registry 结果
  - service 不重新发明 sequence/stale/merge/liveness 判定逻辑
  - summary 继续复用现有 `StorageNodeStatusCode` / `StorageNodeResponseSummary` 风格

## registry 注入与 partial report merge 当前边界

- `StorageNodeService` 现在支持注入 `StorageNodeRegistry`
- heartbeat/report/register service 只调用注入的 registry
- partial report merge 仍由 `StorageNodeRegistry` 负责，service 只做字段转换和结果回填
- 当前还没有 heartbeat/report/register 的 gRPC client 入口，留给 T064

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不保存 payload
- heartbeat/report/register service 只处理 StorageNode data-plane facts

## 是否使用 tests/test_file/test_file.zip

- 否

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_service|storage_heartbeat_registry|storage_node_registry" --output-on-failure 2>&1 | tee tmp/007/t063-storage-node-service-registry.log`
  - PASS
  - 实际匹配到的测试名为 `storage_node_service`、`storage_heartbeat_registry`
  - 日志路径：`tmp/007/t063-storage-node-service-registry.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T063 是 service adapter 和平台无关测试，一般不单列 `T063-WIN`
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未引入平台相关时间源、文件或网络行为

## 是否通过 T063

- 是

## 是否可以进入 T064

- 可以
- 前提：T064 必须复用 T062/T063 已固定的 request 字段语义、sequence/stale 语义和 partial merge contract

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，执行 `007` 任务时需要人工纠偏
- 当前 service 仍直接信任请求携带的 `observed_at_unix_ms` 和 sequence；真正统一 client 时间源、重试和幂等边界要到 T064 收口
- 当前 service 只把 load facts 结构化透传给 registry，不在 T063 内决定 placement/read-side 的最终消费策略

## 是否更新 module-notes.md / AGENTS.md

- 是
- `modules/store/node/module-notes.md`：补充 registry service 入口职责和新增 helper/流程
- `modules/store/node/AGENTS.md`：收口 heartbeat/report/register service 已实现、client 未实现的边界

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - register request 转换 helper
  - heartbeat request 转换 helper
  - health/capacity/load report 转换 helper
  - registry result 到 proto response 转换 helper
  - status mapping helper
  - `StorageNodeService::RegisterStorageNode` 流程
  - `StorageNodeService::UpdateStorageNodeHeartbeat` 流程
  - `StorageNodeService::ReportHealth / ReportCapacity / ReportLoad` 流程

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T063 标记完成，并记录实际影响文件
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：收缩“service 未接线”风险，并补充 T063 后仍保留的 client/time-source/facts-consumer 风险

## common-risk-notes.md 读取结果

- 已读取并维护
- 原有 Windows durability、GC/recovery、registry freshness、placement/read-side 接线等风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T063` service 适配已完成，但 client 时间源、sequence 生成和后续 facts 消费仍待收口
- 删除：无整项删除
- 收缩：
  - `T059` 从“service/client/consumer 都未接线”收缩为“service 已接线，但 client/consumer 未接线”
  - `T061` 从“schema + registry，service/client 未接线”收缩为“schema + registry + service，client/consumer 未接线”
  - `T062` 从“registry 已有但 service/client 未接线”收缩为“registry + service 已有，但 client/consumer 未接线”
