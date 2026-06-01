# T061 Storage Node Heartbeat Proto

## 修改文件

- `proto/storage_node.proto`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t061-storage-node-heartbeat-proto.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `proto/storage_node.proto` 中新增 StorageNode heartbeat/report/register 相关 enum、facts message、request/response message 与 RPC：
  - `RegisterStorageNode`
  - `UpdateStorageNodeHeartbeat`
  - `ReportHealth`
  - `ReportCapacity`
  - `ReportLoad`
- 新增健康、磁盘压力、liveness 枚举，以及 capacity/health/load/failure-domain/facts/snapshot 等消息，复用现有 `StorageNodeStatusCode` 和 `StorageNodeResponseSummary` 风格，不引入新的冲突状态体系。
- 为适配新生成的 gRPC `StubInterface`，同步更新了 `tests/storage_node_client_test.cpp` 中的 fake stub，仅补最小 unimplemented 桩，不实现任何生产 service/client 行为。

## heartbeat / register / report proto 字段和状态语义，说明如何从 T059/T060 contract 和现有 proto 风格推导

- `RegisterStorageNodeRequest`
  - `request_id`
  - `node_id`
  - `endpoint`
  - `observed_at_unix_ms`
  - `facts`
  - 推导依据：T059 的注册幂等/endpoint conflict/初始 capacity + chunk_count 持久化边界；现有 proto 请求风格保持扁平 identity + payload。
- `StorageNodeHeartbeat`
  - `node_id`
  - `endpoint`
  - `sequence`
  - `observed_at_unix_ms`
  - `facts`
  - 推导依据：T059 的 heartbeat sequence、stale heartbeat 忽略、same-sequence 幂等与 `last_seen + timeout` liveness 推导。
- `ReportHealthRequest` / `ReportCapacityRequest` / `ReportLoadRequest`
  - 都包含 `request_id/node_id/endpoint/sequence/observed_at_unix_ms`
  - 分别承载局部 `health` / `capacity` / `load` facts
  - 推导依据：T061 只做 schema，不做 registry；因此既保留全量 heartbeat，也给后续 T063/T064 局部上报入口留出最小协议面。
- `StorageNodeHealthReport`
  - `health`
  - `disk_pressure`
  - `io_error_count`
  - 推导依据：T059 registry contract 和 T060 placement contract 都明确消费 health / disk pressure / IO 错误趋势。
- `StorageNodeCapacityReport`
  - `total_capacity_bytes`
  - `used_capacity_bytes`
  - `available_capacity_bytes`
  - `chunk_count`
  - 推导依据：T059 注册/心跳 contract、US4 spec 和 placement capacity filtering 语义。
- `StorageNodeLoadReport`
  - `active_reads`
  - `active_writes`
  - `queued_ops`
  - `write_admission_overloaded`
  - `read_admission_overloaded`
  - 推导依据：现有 `StorageNodeLoadSnapshot`、T060 placement 过载排除、T045/T066 读副本选择过载降权扩展点。
- `StorageNodeFailureDomain`
  - `zone`
  - `rack`
  - 推导依据：现有 placement candidate 已有 zone/rack placeholder，后续 T065 failure-domain spread 可直接复用。
- `StorageNodeRegistrySnapshot`
  - `node_id`
  - `endpoint`
  - `last_sequence`
  - `last_seen_unix_ms`
  - `liveness`
  - `facts`
  - 推导依据：T059/T060 都依赖控制面推导后的 freshness/liveness 和可供 placement 消费的快照视图；liveness 放在 snapshot 中，避免被误解为节点自报。
- `RegisterStorageNodeResponse`
  - `summary`
  - `created`
  - `idempotent`
  - `snapshot`
  - 推导依据：T059 register 成功/幂等/冲突 contract。
- `StorageNodeFactUpdateResponse`
  - `summary`
  - `accepted_sequence`
  - `applied`
  - `idempotent`
  - `stale_ignored`
  - `snapshot`
  - 推导依据：T059 heartbeat apply/idempotent/stale ignored contract，以及后续 partial report 更新需要的统一反馈风格。

## heartbeat 独立于 Raft heartbeat 的边界

- 本任务只修改 `proto/storage_node.proto`。
- 没有修改 `raft.proto`、`metadata.proto`。
- 新增的 `UpdateStorageNodeHeartbeat` 明确属于 `StorageNodeService` 数据面状态上报，不表达 Raft 选举/复制内部心跳。
- schema 不包含 object committed/deleted 决策，不传 object payload，不暴露本地文件路径。

## 是否修改 CMakeLists.txt

- 否
- 现有 `storage_node_proto` target 已能自动接住 `proto/storage_node.proto` 变更并完成 codegen。

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target storage_node_proto 2>&1 | tee tmp/007/t061-storage-node-heartbeat-proto.log`
  - PASS
- `protoc --proto_path=proto --cpp_out=/tmp/storage_node_proto_check proto/storage_node.proto 2>&1 | tee -a tmp/007/t061-storage-node-heartbeat-proto.log`
  - PASS
- `protoc --proto_path=proto --grpc_out=/tmp/storage_node_grpc_check --plugin=protoc-gen-grpc=$(command -v grpc_cpp_plugin) proto/storage_node.proto 2>&1 | tee -a tmp/007/t061-storage-node-heartbeat-proto.log`
  - PASS
- `ctest --test-dir build/linux -R "storage_heartbeat_registry|store_placement_policy|store_placement_manager" --output-on-failure 2>&1 | tee tmp/007/t061-heartbeat-placement-contract.log`
  - PASS
  - 说明：实际匹配到的测试名为 `store_placement_policy`、`store_placement_manager`、`storage_heartbeat_registry`

## 如果失败：失败原因、失败检查/测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T061 是 proto/schema/codegen 任务，没有新增平台相关运行时行为。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- 本任务不新增 `T061-WIN`。

## 是否通过 T061

- 是

## 是否可以进入 T062

- 可以
- 说明：proto/schema 与 codegen 边界已固定，后续可以进入生产 `StorageNodeRegistry` 设计与实现，但仍需保留 T059/T060/T061 已固定的 contract，不能把本任务结果误当成 registry/service/client/placement 接线已完成。

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，执行 `007` 任务时需要人工纠偏。
- 当前 schema 同时支持全量 heartbeat 和局部 report，但二者在生产 registry 中的 merge 规则、sequence 单调性和最终 liveness 推导仍待 T062-T066 收口。

## 是否更新 module-notes.md / AGENTS.md

- 否

## module-notes.md 是否需要补充 .cpp 关键函数 / helper

- 否
- 本任务只修改 proto 和测试 fake stub，没有修改 `modules/store/*` 生产 `.cpp`。

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T061 完成，并记录实际影响到的 fake stub 同步文件。

## common-risk-notes.md 读取结果

- 已读取并维护。
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045、T049、T055、T056、T057、T059、T060 风险继续保留，并新增 T061 风险记录。

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T061` schema/codegen 已完成，但 partial report merge、liveness 推导、service/client/placement/read-side 接线仍待后续任务收口。
- 删除：无
- 保留：原有风险全部保留；其中 T059 风险已收缩为“proto 已补齐，但生产接线仍未完成”。
