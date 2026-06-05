# T081 Scrub/Repair Proto

## 修改文件

- `proto/storage_node.proto`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t081-scrub-repair-proto.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `proto/storage_node.proto` 中新增：
  - `ScrubChunkRequest`
  - `ScrubChunkFact`
  - `ScrubChunkResult`
  - `ScrubChunkResponse`
  - `RepairChunkRequest`
  - `RepairChunkFact`
  - `RepairChunkResult`
  - `RepairChunkResponse`
- 在 `StorageNodeService` proto service 中新增：
  - `rpc ScrubChunk(ScrubChunkRequest) returns (ScrubChunkResponse);`
  - `rpc RepairChunk(RepairChunkRequest) returns (RepairChunkResponse);`
- 保持现有 `StorageNodeStatusCode`、`StorageNodeResponseSummary`、`StorageChunkChecksum`、`StorageChunkState` 风格，不新增冲突状态体系。
- 只对 `tests/storage_node_client_test.cpp` 的 fake `StubInterface` 做最小同步，补齐新 RPC 的 sync/async 占位，避免 codegen 后接口扩面导致测试编译失败。

## ScrubChunk / RepairChunk proto 字段和状态语义

- `ScrubChunkRequest`
  - 表达目标 chunk 身份和预期事实：
    - `chunk_id/object_id/version/chunk_index`
    - `expected_size`
    - `expected_checksum`
  - 表达执行边界：
    - `timeout_ms`
    - `best_effort_cancel`
    - `verify_checksum`
    - `quarantine_on_corruption`
- `ScrubChunkFact`
  - 表达单副本本地 scrub 结果：
    - `expected_size/observed_size`
    - `expected_checksum/observed_checksum`
    - `state_before/state_after`
    - `checksum_verified`
    - `known_corrupted`
    - `known_missing`
    - `quarantined`
- `ScrubChunkResult`
  - 只表达本地检查结果：
    - `fact`
    - `repair_required`
    - `retryable`
- `ScrubChunkResponse`
  - 使用 `summary.code` 统一表达 `OK / CHECKSUM_MISMATCH / CORRUPTED / NOT_FOUND / IO_ERROR / TIMEOUT / CANCELLED / UNSUPPORTED / INVALID_ARGUMENT` 等状态

- `RepairChunkRequest`
  - 表达 target durable 写入所需的 chunk data-plane 事实：
    - `chunk_id/object_id/version/chunk_index/offset`
    - `expected_size`
    - `expected_checksum`
    - `source_node_id`
    - `source_size`
    - `source_checksum`
    - `source_state`
    - `source_checksum_verified`
    - `payload`
    - `timeout_ms`
    - `best_effort_cancel`
    - `durability`
- `RepairChunkFact`
  - 表达 repair 写入结果与 source/target 事实：
    - `source_node_id`
    - `target_node_id`
    - `expected_size/observed_size`
    - `expected_checksum/observed_checksum`
    - `source_state`
    - `target_state`
    - `source_checksum_verified`
    - `source_unavailable`
    - `target_durable`
    - `already_exists`
- `RepairChunkResult`
  - 只表达 repair data-plane 结果：
    - `fact`
    - `repaired`
    - `retryable`
- `RepairChunkResponse`
  - 使用 `summary.code` 统一表达 `OK / ALREADY_EXISTS / CHECKSUM_MISMATCH / NODE_UNAVAILABLE / DISK_FULL / IO_ERROR / OVERLOADED / TIMEOUT / CANCELLED / INVALID_ARGUMENT` 等状态

## RPC 只传 chunk bytes/facts、不传 object commit 决策的边界

- `ScrubChunk` 只表达 chunk checksum / size / state / quarantine / corrupted 本地检查结果
- `ScrubChunk` 不包含：
  - object committed / deleted 可见性
  - metadata manifest update
  - repair candidate 全局聚合决策
- `RepairChunk` 只表达 source facts、target durable 写入所需 bytes 和结果事实
- `RepairChunk` 不包含：
  - metadata commit
  - manifest update
  - object committed / deleted 决策
  - Raft proposal / snapshot / metadata payload

## 是否修改 CMakeLists.txt

- 否
- 现有 `storage_node_proto` target 已能自动接住 `proto/storage_node.proto` 变更

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007 /tmp/storage_node_proto_check /tmp/storage_node_grpc_check`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target storage_node_proto 2>&1 | tee tmp/007/t081-scrub-repair-proto.log`
  - PASS
- `protoc --proto_path=proto --cpp_out=/tmp/storage_node_proto_check proto/storage_node.proto 2>&1 | tee -a tmp/007/t081-scrub-repair-proto.log`
  - PASS
- `protoc --proto_path=proto --grpc_out=/tmp/storage_node_grpc_check --plugin=protoc-gen-grpc=$(command -v grpc_cpp_plugin) proto/storage_node.proto 2>&1 | tee -a tmp/007/t081-scrub-repair-proto.log`
  - PASS
- `ctest --test-dir build/linux -R "storage_scrub_repair|storage_rebalance|storage_node_service|storage_node_client" --output-on-failure 2>&1 | tee tmp/007/t081-scrub-repair-contract.log`
  - PASS
  - 实际匹配到的测试名为：
    - `storage_scrub_repair`
    - `storage_rebalance`
    - `storage_node_service`
    - `storage_node_client`
  - 日志路径：
    - `tmp/007/t081-scrub-repair-proto.log`
    - `tmp/007/t081-scrub-repair-contract.log`

## 如果失败：失败原因、失败检查/测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T081 是 proto/schema/codegen 任务
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- Windows 上的 codegen/runtime 兼容性仍待后续实机验证

## 是否通过 T081

- 是

## 是否可以进入 T082

- 可以
- T082 应实现 `StorageNodeService::ScrubChunk` / `StorageNodeClient::ScrubChunk` 与 `RepairChunk` 适配，不要把本轮 proto/codegen 任务扩展成生产 manager 或 repair copy flow

## 当前任务发现的不合理点 / 警告 / 风险

- `RepairChunkRequest` 现允许直接承载 chunk bytes；后续若没有单独的大报文/streaming/timeout 边界，仍可能放大 payload 体积与传输风险。
- 当前新增 RPC 只完成 schema/codegen，没有任何生产 service/client 语义；不要把 fake stub 同步误读成真实 RPC 已可用。
- 当前 schema 没有引入 metadata commit / manifest update 字段；后续实现时必须继续保持 object commit 决策不泄漏到 data-plane RPC。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只修改 proto、测试桩和文档，没有修改生产模块说明或协作规则

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T081 完成并记录真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T081 风险条目，保留 service/client 未实现、RepairChunk payload 边界、Windows 待验证等未解风险

## common-risk-notes.md 读取结果

- 已读取
- 未删除生产 ScrubChunk service/client、RepairChunk service/client、ScrubManager、RepairManager、RebalanceManager、repair task persistence、read-side repair、Windows 实机验证等既有风险
- prerequisites 脚本误指向 006 的问题继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T081`，记录 fake stub 同步、RepairChunk payload 边界、object commit 决策隔离和生产 service/client 未实现风险
- 删除：
  - 无
- 保留：
  - 生产 ScrubChunk service/client、RepairChunk service/client、ScrubManager、RepairManager、RebalanceManager、repair task persistence、read-side repair、Windows 实机验证等后续风险继续保留
