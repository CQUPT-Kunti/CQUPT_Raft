# T052 Preserve Existing Transfer Path While Allowing Future Placement To Include Dynamic StorageNode

## 做了什么

本任务只在 `modules/store/transfer/object_transfer.cpp` 收口 upload/download 的 transfer 兼容边界：

1. 保持现有 upload / commit / download 路径不变。
2. upload 侧的 StorageNode discovery 不再在发现阶段过早按 `desired_replica_count` 截断。
3. upload 侧 discovery 的最小容量门槛从“整对象大小”收紧为“本次实际最大 chunk 大小”。
4. 当未来 `CreateWritePlan` 开始返回 `chunk_plan.candidate_nodes` 时，transfer 会优先按这些候选节点解析 endpoint，再用其余 live/writable 节点补齐，避免因为 discovery 截断把动态注册节点丢掉。
5. download 路径保持 committed manifest 驱动，不因为新 StorageNode 出现而改写旧 manifest 或切换对象事实来源。

本任务没有实现旧对象 rebalance，没有修改已 committed manifest，没有修改 Raft membership / quorum / election，也没有把 StorageNode join 写入 Raft log。

## 修改文件

- `modules/store/transfer/object_transfer.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t052-preserve-existing-transfer-path-while-allowing-future-placement-to-include-dynamic-storagenode.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## transfer path 如何保持兼容

upload 主流程没有变：

1. 第一遍 bounded chunk 读取并计算 checksum
2. `DiscoverMetadata`
3. `CreateWritePlan`
4. `DiscoverStorage`
5. 第二遍 bounded chunk 读取并 `WriteChunk`
6. `CommitObject`
7. 返回 committed manifest 或 fallback committed facts

download 主流程也没有变：

1. `DiscoverMetadata`
2. `GetObjectManifest`
3. 只接受 committed manifest
4. `DiscoverStorage`
5. 按 manifest 的 `replica_nodes` 读取 chunk
6. 校验 chunk checksum / size
7. 校验最终对象 checksum

以下语义保持不变：

- upload 第二遍 reread + checksum 校验
- `minimum_successful_writes`
- failed upload cleanup candidate
- download 只按 committed manifest 读
- checksum / size / manifest layout 检查

## 如何允许未来 placement 使用动态 StorageNode

当前上传侧以前有两个会限制未来动态节点的点：

1. Storage discovery 在发现阶段就按 `desired_replica_count` 截断。
2. 发现时用整对象大小作为 `minimum_available_capacity_bytes`，对 chunked upload 过于保守。

本任务改成：

1. upload discovery 阶段拉取当前 live/writable 的完整候选集合，不在 discovery 阶段截断。
2. discovery 的 `minimum_available_capacity_bytes` 改为本次上传实际 `prepared_chunks` 的最大 chunk 大小。
3. `ResolveChunkTargets(...)` 的行为改为：
   - 若未来 write plan 带 `candidate_nodes`，先按这些 node_id 顺序解析 endpoint。
   - 如果计划节点不足以满足 `desired_replica_count`，再从其余 live/writable discovery 结果里补齐。
   - 如果最终仍不足，再明确失败。
4. 若当前 metadata adapter 仍然不返回 `TransferWritePlan.chunks`，则继续走现有 fallback：按 live/writable discovery 结果排序后选取目标节点。

这样：

- 当前静态 6 StorageNode 基线路径不退化。
- 未来动态注册并被 ViewNode 观测到的 StorageNode，只要通过 live/writable 过滤，就能进入后续新对象写入候选。
- 旧对象不会被重写，因为 download 仍然只按 committed manifest 的 `replica_nodes` 读取。

## 是否确认不修改旧 committed manifest

确认。

本任务没有修改：

- `CommitObject` 的 manifest 提交语义
- committed manifest 的读取语义
- download 的 manifest authority

动态 StorageNode 只影响未来新对象写入候选，不会回写已有对象的 `replica_nodes`。

## 测试情况

本任务没有新增测试文件，也没有弱化现有测试。

原因：

- 当前仓库没有现成的 fake / mock `MetadataTransferClient` 测试支撑，而 `ObjectTransfer` upload 逻辑又依赖真实 metadata transfer client 的非 virtual 方法。
- 因此本任务优先通过现有 integrated/storage 入口做回归，确认 transfer 相关核心路径没有退化。

本任务依赖的现有回归覆盖包括：

- `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest`
- 现有 integrated object storage e2e / quorum / recovery / concurrency 相关用例
- `storage_heartbeat_registry`

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e test_storage_heartbeat_registry > tmp/test-logs/t052-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorage|StorageHeartbeatRegistry" --output-on-failure > tmp/test-logs/t052-ctest.log 2>&1
ctest --preset debug-tests -R "storage_heartbeat_registry" --output-on-failure >> tmp/test-logs/t052-ctest.log 2>&1
```

## 验证结果

- PASS
- build：
  - `integrated_object_storage_e2e`
  - `test_storage_heartbeat_registry`
- tests：
  - `IntegratedObjectStorageE2ETest.DynamicStorageNodePlacementSeesNewNodeWithoutRewritingCommittedManifest` 通过
  - `IntegratedObjectStorageE2ETest.ManifestVisibilityPendingHiddenCommittedVisible` 通过
  - 其余命中的 integrated object storage 相关启用用例全部通过
  - `storage_heartbeat_registry` 通过
- disabled 用例保持 disabled，没有被本任务改动
- 日志：
  - `tmp/test-logs/t052-build.log`
  - `tmp/test-logs/t052-ctest.log`

## 结论

- 状态：PASS
- 已满足 T052 勾选条件
- 可以进入 T053
