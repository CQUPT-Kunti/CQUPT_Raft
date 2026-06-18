# T005-B Quorum And Slow Replica Handling Report

## 已读取的 T005-A 报告路径

- `specs/010-object-storage-config-industrialization/task-reports/t005a-bounded-parallel-replica-fanout.md`

## 修改文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/module-notes.md`
- `tests/transfer_write_plan_test.cpp`
- `specs/010-object-storage-config-industrialization/tasks.md`

## minimum success 聚合规则

- chunk commit-eligibility 的唯一条件是：
  - `durable_success_count >= minimum_successful_writes`
- durable success 只统计 `write_result.ok()` 且 `node_id` 唯一的 replica
- duplicate success / duplicate `node_id` 不会重复计数
- planned、attempted、failed、uncertain 节点都不会进入 durable success 集合

## quorum 达成后的处理方式

- 当前实现不会在 quorum 达成后立刻进入下一 chunk
- 语义是：
  - 先把 chunk 标记为 commit-eligible
  - 再等待当前 chunk 已启动的 replica task 在各自 deadline 内完成收尾
  - 收尾结束后再冻结 manifest facts 并进入下一 chunk
- 后续普通失败不会把已经满足 quorum 的 chunk 改成失败

## slow replica 的 deadline / 收尾方式

- 每个 replica write 都带显式 `StorageTaskContext.timeout_ms`
- 当前 deadline 由 `modules/store/transfer/object_transfer.cpp` 内部常量 `kReplicaWriteTimeoutMs` 控制
- 已启动任务都通过 `BoundedStorageExecutor` worker 执行
- upload 不使用 detached task，也不依赖不可移植的线程取消
- 所有已启动任务都在 upload session 返回前完成或在 deadline 内返回 timeout 结果

## payload 生命周期

- 每个 chunk 的 second-pass payload 会封装成单份 `shared_ptr<const std::string>`
- fan-out task 共享这份只读 owner
- upload 主线程在当前 chunk 所有已启动任务完成前不会进入下一 chunk，因此不会发生 payload use-after-free

## retryable、non-retryable、timeout / uncertain 的表达

- retryable / timeout / transport-uncertain 通过 `StorageTransferWriteResult.retryable` 与 `StorageNodeStatusCode::kTimeout` 表达
- `IsUncertainReplicaWriteResult(...)` 把 retryable 和 timeout 统一视为 uncertain durable state
- quorum 未达成且存在 uncertain 节点时，chunk 失败优先暴露 timeout / uncertain 语义
- non-retryable failure 继续保留原始映射，例如 checksum mismatch -> `ObjectTransferStatusCode::kChecksumMismatch`

## manifest 最终冻结规则

- 只有 actual durable success nodes 会进入 `durable_replicas`
- 只有 commit-eligible chunk 才会转成 `TransferCommittedChunk`
- `CommitObject` 只消费 `result.committed_chunks`
- timeout / unknown / failed / duplicate-success-collapsed 节点不会写入 manifest

## cleanup / degraded / repair candidate facts

- quorum 未达成但存在已 durable success 的 chunk，仍会生成 failed-upload cleanup candidates
- 只要存在 uncertain replica result，就会把 `cleanup_candidate_possible` 置为 true
- uncertain risk 通过 cleanup candidates + diagnostics 保留给后续 cleanup / repair 方向

## 新增测试

- `ObjectTransferWritePlanTest.UploadSucceedsAfterQuorumWithBoundedSlowReplicaTimeoutAndExcludesTimeoutNode`
  - 两个成功、一个 slow timeout，chunk 在有界时间内成功
  - timeout 节点不进入 manifest
- `ObjectTransferWritePlanTest.UploadFailsWhenUniqueDurableSuccessesDoNotReachMinimumAndTracksUncertainFacts`
  - 一个成功、一个 timeout、一个 non-retryable failure，chunk 失败
  - uncertain risk 进入 cleanup / diagnostics
- `ObjectTransferWritePlanTest.UploadDoesNotDoubleCountDurableSuccessWhenReplicaResponsesCollapseToSameNodeId`
  - duplicate success / duplicate `node_id` 不会重复计数
- 保留并通过 T004 authority/no-fallback 测试与 T005-A bounded fan-out / chunk-serial tests

## 验证命令和结果

```bash
cmake --build --preset debug-ninja-low-parallel
ctest --test-dir build/linux --output-on-failure -R "transfer_write_plan|storage_upload_integration|storage_upload_coordinator"
```

- 结果：PASS

## 状态

- PASS

## 是否勾选 T005

- 是
- 仅在本阶段通过验证后，将原始 `T005` 从 `[ ]` 改为 `[X]`

## T006 可以依赖的并发边界

- chunk 内 replica fan-out 已是 bounded parallel，worker 上限固定且不无界建线程
- chunk 间仍严格串行
- 每个已启动 replica task 都带 bounded timeout，并在 upload 返回前完成收尾
- manifest 冻结仍发生在 `CommitObject` 之前，且只基于 actual durable success facts
