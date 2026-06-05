# T085 RepairChunk Copy Flow

## 修改文件

- `modules/store/node/storage_node_service.h`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/module-notes.md`
- `modules/store/maintenance/repair_manager.h`
- `modules/store/maintenance/repair_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_node_service_test.cpp`
- `tests/storage_node_client_test.cpp`
- `tests/storage_scrub_repair_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t085-repair-chunk-copy-flow.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `StorageNodeService::RepairChunk`，把 proto request 收口为本地 source facts 校验 + target durable write。
- 新增 `StorageNodeClient::RepairChunk` 及本地 request/response/options 映射，补齐 deadline 和 gRPC status 映射。
- 扩展生产 `RepairManager`，新增 `RunTask()`、`source_reader`、`target_writer`，让 repair task 能驱动最小 data-plane copy flow。
- 扩展 `storage_node_service`、`storage_node_client`、`storage_scrub_repair` 测试，固定 target durable、幂等成功、冲突、坏源、坏目标、失败记账等 contract。

## RepairChunk service/client 字段映射和状态语义

- request 侧固定传递 `chunk_id/object identity`、`source_node_id`、`expected_checksum/expected_size`、`source_checksum/source_size/source_state/source_checksum_verified`、`payload`、`timeout_ms`、`best_effort_cancel`、`durability`。
- service 侧要求 source facts 完整且已 checksum verified；missing/deleted source 返回 `NOT_FOUND`，quarantined/corrupted source 返回 `CORRUPTED`，checksum/size mismatch 返回 `CHECKSUM_MISMATCH` 或 `INVALID_ARGUMENT`。
- target `WriteChunk()` durable 成功后返回 `repaired=true`、`target_durable=true`；`already_exists` 且 checksum/size 一致视为幂等成功；`already_exists` 但内容不一致返回 `CONFLICT` / `CHECKSUM_MISMATCH`。
- client 侧把 proto summary/result 和 gRPC status 映射回本地 `StorageNodeStatusCode`、expected/observed checksum/size、source/target state、`already_exists/target_durable/repaired/retryable`。

## RepairManager copy flow 输入、输出和 task 状态推进语义

- 输入：已有 `RepairTask`、registry snapshot、注入的 `source_reader` / `target_writer`、task context。
- 流程：`MarkTaskRunning()` -> source/target registry revalidate -> source read -> checksum/size verify -> target durable write -> `CompleteTask()` 或 `FailTask()`。
- 输出：`RepairTaskRunResult`，包含 source/target、source checksum/size、retryable、错误详情和最新 task snapshot。
- 状态推进：只有 target durable 成功才进入 `Completed`；失败按 retryable 进入 `RetryPending` 或 `Failed`；不会伪装 metadata manifest 已更新。

## target durable before task completed 当前边界

- `RepairManager::RunTask()` 只有在 target writer 返回 durable success 后才调用 `CompleteTask()`。
- target write 失败、source 校验失败、registry revalidate 失败都不会把 task 标成 completed。
- `already_exists` 只有在 checksum/size 与期望一致时才作为幂等 completed。

## metadata manifest coordination 当前边界

- 本任务不实现 metadata manifest coordination。
- `RepairChunk` 和 `RepairManager` 都不更新 metadata manifest，不发布 replica facts，不做 source cleanup。
- task `Completed` 仅表示 target data-plane durable complete，不代表 object manifest 已协调。

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot / state machine
- payload 只在 repair data-plane RPC / manager copy flow 中用于 checksum 校验和 target durable write

## 是否使用 tests/test_file/test_file.zip

- 否
- 测试 payload 全部使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：`tmp/007/t085-build.log`
- `ctest --test-dir build/linux -R "storage_scrub_repair|repair_manager|storage_node_service|storage_node_client|repair_chunk" --output-on-failure 2>&1 | tee tmp/007/t085-repair-chunk-copy-flow.log`
  - PASS
  - 实际匹配到的测试名：
    - `storage_scrub_repair`
    - `storage_node_service`
    - `storage_node_client`
  - 日志路径：`tmp/007/t085-repair-chunk-copy-flow.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T085 涉及 target durable write / file copy / already_exists / conflict 语义。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- Windows 下的 file copy / publish / rename / sharing violation / durable 语义仍待后续实机验证。

## 是否通过 T085

- 是

## 是否可以进入 T086

- 可以
- T086 应继续做 under-replicated detection，不要把本轮结果扩展成 metadata manifest coordination、read-side repair 或 rebalance。

## 当前任务发现的不合理点 / 警告 / 风险

- `RepairManager` 当前仍没有 task persistence / restart resume。
- source/target 选择和再次执行依赖运行时 registry snapshot 与调用方 candidate 新鲜度。
- target durable 与 metadata manifest coordination 之间仍有时间窗。
- `RepairChunkRequest` 直接承载 payload，large payload / streaming / 背压边界仍待后续收口。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/node/module-notes.md`
- 更新了 `modules/store/maintenance/module-notes.md`
- 未更新 `AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T085 完成并记录真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T085 风险条目，保留 metadata manifest coordination、task persistence、Windows 实机验证等未解风险

## common-risk-notes.md 读取结果

- 已读取
- 保留了 under-replicated detection 未实现、RepairManager persistence 未实现、metadata manifest coordination 未实现、read-side repair 未实现、RebalanceManager 未实现、Windows 实机验证待完成、metadata / registry facts 新鲜度风险，以及 `.specify/scripts/bash/check-prerequisites.sh` 误指向 006 的风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T085`，记录 target durable 与 metadata 协调时间窗、task persistence / restart resume 缺失、registry snapshot freshness、large payload/streaming 和 Windows 实机语义待验证
- 删除：
  - 无
- 保留：
  - under-replicated detection、RepairManager persistence、metadata manifest coordination、read-side repair、RebalanceManager、Windows 实机验证、metadata / registry facts 新鲜度等后续风险继续保留
