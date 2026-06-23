# T007-A Manifest-Scoped Same-Chunk Replica Fallback Read Report

## 任务目标

在 T006 已完成 upload manifest durable replica facts 的基础上，建立 production download 的 manifest-scoped fallback 主路径。

本阶段只覆盖：

- 每个 chunk 只使用 committed manifest 中该 chunk 的 `replica_nodes`
- 首选 replica 失败后，继续尝试同 chunk 下一个 manifest replica
- 不从 discovery、固定 replica group、其他 chunk 或 manifest 外 StorageNode 推断 fallback
- 不同 chunk 可拥有不同 replica set
- 保持既有 chunk layout、size、checksum 校验边界

## 修改文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/module-notes.md`
- `tests/storage_read_integration_test.cpp`
- `tests/CMakeLists.txt`

## 生产代码改动

### download fallback 边界

在 `BasicDownloadTransferSession::Execute(...)` 中：

- 旧逻辑：对每个 chunk 通过 `ResolveReplicaTarget(...)` 解析一个可读 target，直接调用一次 `StorageTransferClient::ReadChunk(...)`
- 新逻辑：对每个 chunk 通过 `ResolveManifestReplicaTargets(...)` 解析该 chunk manifest `replica_nodes` 对应的 target 列表，并按该列表顺序逐个尝试读取

### authority 约束

- candidate list 仅来自 `TransferCommittedChunk::replica_nodes`
- discovery 只做 `node_id -> endpoint` 解析，不会扩展候选集合
- 如果 manifest 节点都无法解析到 endpoint，下载失败并保持 `DiscoveryUnavailable`
- manifest 外健康节点即使存在于 `DiscoverStorage` 返回中，也不会进入尝试列表

### fallback 语义

- 当前 replica read 失败时，记录一次 chunk-scoped diagnostic
- 只要同 chunk 还有后续 manifest replica，就继续尝试
- 第一条成功读结果继续经过现有 payload size / chunk checksum / object checksum 路径
- 所有 same-chunk manifest replicas 都失败时，返回该 chunk 的失败

## 测试覆盖

新增 production download integration case：

1. `ProductionDownloadFallsBackWithinSameChunkManifestReplicaList`
   - 首选 manifest replica `NotFound`
   - 第二个 manifest replica 成功
   - 验证按 `a -> b` 对每个 chunk fallback

2. `ProductionDownloadUsesPerChunkManifestReplicaSetsOnly`
   - 不同 chunk 使用不同 manifest replica set
   - 验证 chunk 0 命中 `replica-a`，chunk 1 命中 `replica-c`

3. `ProductionDownloadDoesNotAttemptManifestExternalReadableNodes`
   - discovery 中存在 manifest 外健康节点 `replica-extra`
   - 验证 download 不会尝试该节点

4. `ProductionDownloadAllowsNeutralFallbackWhenObservedFactsAreMissing`
   - 仅依赖 manifest + discovery 解析 endpoint
   - 首选失败后，后续 manifest 节点可作为中性 fallback 成功

既有 `storage_read_integration` / `metadata_manifest` 覆盖保持不回归。

## 未在本阶段实现

- checksum mismatch 的完整 diagnostics 清理
- 所有副本失败的最终聚合错误格式
- object checksum 发布保护重构
- repair-B 后台逻辑或 manifest 更新
- 非 manifest fallback
- T007-B 的诊断整理工作

## tasks.md 状态

- 保持原始 `T007` 为未勾选
- 未新增 `T007-A` 到 `tasks.md`

## 验证

建议命令：

```bash
ctest --test-dir build/linux --output-on-failure -R "^(storage_read_integration|metadata_manifest)$"
```

本次实际验证：

```bash
cmake --build --preset debug-ninja-low-parallel --target test_storage_read_integration test_metadata_manifest
ctest --test-dir build/linux --output-on-failure -R "^(storage_read_integration|metadata_manifest)$"
ctest --test-dir build/linux --output-on-failure -R "^MetadataManifestTest\\."
```

结果：

- `storage_read_integration`: PASS
- `MetadataManifestTest.MetadataProtoCarriesChunkRefsWithoutPayloadBytes`: PASS
- `MetadataManifestTest.MetadataStateMachineSnapshotRoundTripPreservesChunkRefsWithoutPayloadMarker`: PASS
