# T082 ScrubChunk Service/Client

## 修改文件

- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/module-notes.md`
- `tests/storage_node_service_test.cpp`
- `tests/storage_node_client_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t082-scrub-chunk-service-client.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `StorageNodeService::ScrubChunk`，使用注入的 `ChunkStore::StatChunk()` 走 `pre-stat -> verify-stat -> post-stat` 最小生产链路。
- 新增 `StorageNodeClient::ScrubChunk` 及本地 request/response/options 结构，补齐 proto request/response 与本地 scrub 事实的映射。
- 新增/扩展 service/client 测试，覆盖 healthy、missing、corrupted、quarantined、checksum mismatch、gRPC timeout/cancelled/unavailable/invalid_argument，以及 client + real service + real store 的健康/坏块链路。

## ScrubChunk service/client 字段映射和状态语义

- service request 侧：
  - `chunk_id/object_id/version/chunk_index` 解析为本地 `StatChunkRequest.chunk_id`
  - `expected_size/expected_checksum` 保留给 service 侧二次比较
  - `timeout_ms/best_effort_cancel` 当前只作为 RPC contract 字段
  - `verify_checksum` 当前在 service 内固定走 checksum verify 路径
- service response 侧：
  - healthy live chunk -> `OK`，返回 expected/observed checksum/size，`state_before=LIVE`，`state_after=LIVE`
  - missing chunk -> `NOT_FOUND`，`known_missing=true`
  - 本地坏块/校验失败 -> `CORRUPTED`，`known_corrupted=true`，如触发 T072 quarantine 则 `quarantined=true`
  - 已 quarantined chunk -> 归一成不可作为 healthy source 的 `CORRUPTED`
  - expected checksum/size 与已验证结果不一致 -> `CHECKSUM_MISMATCH`
- client 侧：
  - proto `ScrubChunkResponse` -> `StorageNodeClientScrubChunkResponse`
  - gRPC `DEADLINE_EXCEEDED/CANCELLED/UNAVAILABLE/INVALID_ARGUMENT` -> 本地 `TIMEOUT/CANCELLED/NODE_UNAVAILABLE/INVALID_ARGUMENT`

## healthy / missing / corrupted / quarantined 当前边界

- healthy：必须经过 checksum verify，才返回 `OK`
- missing：不伪装成成功，返回 `NOT_FOUND + known_missing`
- corrupted：复用 `LocalDiskChunkStore::StatChunk(verify_checksum=true)` 的 T072 语义，发现文件大小或 checksum 异常时触发 quarantine 并返回 `CORRUPTED`
- quarantined：当前不再尝试把它当 healthy source，service 直接归一为 `CORRUPTED`

## deadline / cancellation 当前边界

- client 会把 `timeout_ms` 写入 proto request，并映射到 `grpc::ClientContext` deadline
- `best_effort_cancel` 只透传 contract 字段，不伪装成运行中取消传播
- service 当前只消费请求字段并保持边界，不新增后台取消协议

## 是否调用 metadata / Raft；是否保存 payload

- 否
- `ScrubChunk` 只读本地 chunk facts，不修改 metadata manifest，不调用 Raft，不把 payload 写入 metadata / Raft

## 是否使用 tests/test_file/test_file.zip

- 否
- 新增的 T082 scrub 测试 payload 全部使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_service|storage_node_client|storage_scrub_repair|scrub_chunk" --output-on-failure 2>&1 | tee tmp/007/t082-scrub-chunk-service-client.log`
  - PASS
- 日志路径：
  - `tmp/007/t082-scrub-chunk-service-client.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T082 是 service/client adapter 和平台无关测试
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 因为真实坏块测试仍涉及 quarantine 文件语义，Windows rename / quarantine 实机行为继续保留为待验证风险

## 是否通过 T082

- 是

## 是否可以进入 T083

- 可以
- T083 应进入生产 `ScrubManager` bounded background queue，不要把本轮 service/client 适配误读成 manager 已完成

## 当前任务发现的不合理点 / 警告 / 风险

- `ScrubChunkRequest.quarantine_on_corruption` 当前不能关闭 `LocalDiskChunkStore::StatChunk(verify_checksum=true)` 的 T072 quarantine 行为。
- expected checksum/size mismatch 当前仍是 service 侧二次比较结果，和生产 metadata manifest / registry facts 的 freshness 协议尚未接线。
- `best_effort_cancel` 仍只是 request/deadline 边界提示，不代表运行中取消传播。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/node/module-notes.md`
  - 记录了 `ScrubChunk` request/response helper、checksum/size/state 映射、corrupted/quarantined/missing 语义、gRPC status/deadline 边界，以及 service/client 流程
- 未更新 `AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 标记 T082 完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 更新 T081 风险描述，并新增 T082 的 quarantine 开关、deadline/cancel、freshness 边界风险

## common-risk-notes.md 读取结果

- 已读取
- 保留了生产 `RepairChunk` service/client、ScrubManager、RepairManager、RebalanceManager、read-side repair、repair task persistence、Windows 实机验证等未解风险
- `.specify/scripts/bash/check-prerequisites.sh` 误指向 006 的风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T082`，记录 `quarantine_on_corruption` 不能抑制 T072 quarantine、`best_effort_cancel` 仍不代表运行中取消传播，以及 scrub fact freshness 尚未接线
- 删除：
  - 无
- 保留：
  - `RepairChunk` service/client、生产 ScrubManager/RepairManager/RebalanceManager、repair task persistence、read-side repair、Windows 实机验证等后续风险继续保留
