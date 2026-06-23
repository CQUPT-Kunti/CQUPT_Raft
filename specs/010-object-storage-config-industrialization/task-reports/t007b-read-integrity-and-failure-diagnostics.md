# T007-B Read Integrity And Failure Diagnostics Report

## 任务目标

在 T007-A 已建立 manifest-scoped same-chunk replica fallback 的基础上，补全下载正确性边界：

- chunk replica 返回 checksum mismatch、size mismatch、corruption 或 invalid payload 时继续同 chunk fallback
- 所有同 chunk manifest replicas 都失败时返回明确的 chunk-scoped 聚合错误
- 所有 chunk 成功后继续校验最终 object checksum
- final output 只在全部校验成功后发布
- 保留 repair-ready diagnostics，但不执行 repair、manifest 更新或后台任务

## 修改文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/module-notes.md`
- `tests/storage_read_integration_test.cpp`

## 生产代码改动

### 1. chunk replica fallback 从“读失败”扩展到“读后校验失败”

在 `BasicDownloadTransferSession::Execute(...)` 的逐 chunk 下载循环中：

- 继续只从当前 chunk committed manifest 的 `replica_nodes` 构造 candidate list
- 对每个 manifest replica 统一执行：
  - `StorageTransferClient::ReadChunk(...)`
  - payload size 校验
  - chunk checksum 校验
- 只要某个 replica 在上述任一步失败，就把它视为当前 replica attempt failure，并继续尝试同 chunk 的下一个 manifest replica
- 只有某个 replica 完整通过上述校验后，才会把 payload 写入临时输出文件

### 2. 所有同 chunk replicas 失败时返回聚合错误

新增内部 attempt failure 聚合逻辑：

- 每个失败 replica 都保留：
  - `node_id`
  - `endpoint`
  - failure classification
  - detail
  - retryable
- 聚合错误 detail 至少包含：
  - `chunk index`
  - attempted node ids
  - 每个 node 的 failure classification
  - timeout / missing / checksum mismatch / size mismatch / corruption 等事实

状态聚合策略保持显式语义，不把所有情况压扁成 `kInternalError`。

### 3. repair-ready diagnostics 保留，但不触发 repair

当前 download 对以下 replica-level 事实都会记录 diagnostic：

- `missing`
- `timeout`
- `retryable failure`
- `corruption`
- `size mismatch`
- `checksum mismatch`

这些 facts 只保留在 transfer 结果里，未触发：

- repair write
- metadata manifest 更新
- 后台 repair task

### 4. 最终 object checksum 与 output publish 保护

- 所有 chunk 完成后，仍需执行最终 object checksum 校验
- object checksum mismatch 时直接失败
- 失败时清理 `.part` 临时文件
- 只有所有 chunk 和最终 object checksum 都通过后，才执行 `rename` 发布最终 output

## 测试覆盖

新增 production download integration case：

1. `ProductionDownloadFallsBackAfterChunkChecksumMismatch`
   - 首个 manifest replica 返回坏 payload
   - transfer 侧 chunk checksum 校验失败后 fallback 到第二个 replica
   - 下载成功，diagnostics 保留 checksum mismatch facts

2. `ProductionDownloadFallsBackAfterChunkSizeMismatch`
   - 首个 manifest replica 返回 size mismatch payload
   - fallback 到第二个 replica 成功

3. `ProductionDownloadAggregatesAllManifestReplicaFailuresAndCleansOutput`
   - 同 chunk replicas 分别触发 timeout / missing / checksum mismatch
   - 下载失败
   - error detail 带 chunk index、attempted nodes 和 failure 分类
   - 不尝试 manifest 外节点
   - 不发布部分 output

4. `ProductionDownloadRejectsFinalObjectChecksumMismatchWithoutPublishingOutput`
   - 所有 chunk 都能成功读取
   - 最终 object checksum 与 manifest facts 不一致
   - 下载失败，最终 output 与临时 output 都被清理

5. `ProductionDownloadRetainsRepairReadyDiagnosticsWithoutRepairWrites`
   - 保留 missing/corruption diagnostics
   - 不触发 repair write 或 metadata 更新

既有 `transfer_write_plan`、`storage_read_integration`、`MetadataManifestTest.*` 回归保持通过。

## tasks.md 状态

- 本次在所有 targeted tests PASS 后，将原始 `T007` 从 `[ ]` 更新为 `[X]`
- 未新增 `T007-B` 到 `tasks.md`

## 验证

本次实际验证命令：

```bash
cmake --build --preset debug-ninja-low-parallel --target test_storage_read_integration test_metadata_manifest test_transfer_write_plan
ctest --test-dir build/linux --output-on-failure -R "^(storage_read_integration|metadata_manifest|transfer_write_plan)$"
ctest --test-dir build/linux --output-on-failure -R "^MetadataManifestTest\\."
```

结果：

- `transfer_write_plan`: PASS
- `storage_read_integration`: PASS
- `MetadataManifestTest.MetadataProtoCarriesChunkRefsWithoutPayloadBytes`: PASS
- `MetadataManifestTest.MetadataStateMachineSnapshotRoundTripPreservesChunkRefsWithoutPayloadMarker`: PASS

日志文件：

- `tmp/test-logs/t007b_verify/ctest_t007b_targeted.log`
- `tmp/test-logs/t007b_verify/ctest_metadata_manifest.log`

## 后续边界

T007 完成后可以进入 T008，但 T008 仍需要实现：

- repair decision B 所需的 placement/repair-ready facts 消费
- 不补回原节点前提下的新目标选择
- 后续 repair 方向与 manifest/maintenance 的衔接
