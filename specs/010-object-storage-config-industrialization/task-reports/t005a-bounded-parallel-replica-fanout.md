# T005-A Bounded Parallel Replica Fan-Out Report

## 修改文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/module-notes.md`
- `tests/transfer_write_plan_test.cpp`

## 原串行 replica write 位置

- `modules/store/transfer/object_transfer.cpp`
- upload second pass 的 chunk 写入循环原先直接 `for (const auto &target : chunk_targets)` 串行调用 `storage_client_->WriteChunk(...)`
- `minimum_successful_writes`、durable replica 收集和 manifest facts 也在该串行循环内聚合

## 使用的 bounded executor 或并发机制

- 复用 `modules/store/runtime/storage_executor.h` / `.cpp` 的 `BoundedStorageExecutor`
- 在单次 upload session 的 second pass 内创建一个 session-scoped fan-out executor
- 每个 selected replica write 作为一个 executor task 提交
- 每个 chunk 等待本 chunk 已启动的全部 replica task 完成后再继续

## fan-out 上限来源

- `modules/store/transfer/object_transfer.cpp` 内部常量 `kMaxReplicaFanoutWorkers`
- 当前 T005-A 实现值为 `2`
- 实际 worker 数 = `min(selected replica count, kMaxReplicaFanoutWorkers)`
- executor queue capacity 当前按 `desired_replica_count` 建立，用于保证 selected replica tasks 都能进入同一个 bounded queue，而不是无界起线程

## payload ownership 和生命周期

- second pass 读到的 `chunk.payload` 会在 fan-out 前转成单份 `shared_ptr<const std::string>` 持有
- 每个 replica task 只共享这份只读 payload owner
- upload 主线程会等待当前 chunk 的全部已启动 replica task 完成后再进入下一个 chunk，避免 payload 提前释放
- 当前 `StorageTransferWriteRequest` 仍是 request-owned payload string；因此 adapter 边界内仍会发生每 RPC 的请求载荷构造，这不是 T005-A 继续扩面的目标

## 结果聚合方式

- 每个 task 的 `StorageTransferWriteResult` 按 selected replica 的稳定索引写入结果槽位
- 主线程等待所有已启动 task 完成后，按 selected replica 顺序聚合：
  - diagnostics
  - first durable result
  - durable replica nodes
  - last failure facts
- 聚合不依赖任务完成顺序

## durable success 如何进入 manifest

- 只有 `write_result.ok()` 的 selected replica 会进入 `durable_replicas`
- `durable_replicas.size() >= minimum_successful_writes` 时才生成 `TransferCommittedChunk`
- `CommitObject` 只提交 `result.committed_chunks`
- manifest 的 replica nodes 只来自 actual durable success nodes，不包含 planned-but-failed 节点

## 当前是否仍等待全部 replica 完成

- 是
- T005-A 明确保留“等待当前 chunk 全部已启动 replica task 完成后再做判断”的边界
- 未实现 quorum 达成后的提前返回

## 新增测试

- `ObjectTransferWritePlanTest.UploadFansOutSelectedReplicasWithBoundedOverlapAndStableManifestAggregation`
  - 验证同一 chunk 的 replica write 确实重叠
  - 验证 fan-out 峰值受当前上限 `2` 约束
  - 验证完成顺序乱序时 manifest 仍按 stable selected order 聚合 durable successes
- `ObjectTransferWritePlanTest.UploadDoesNotCommitWhenSelectedReplicasDoNotReachMinimumWritesAndDoesNotUseExtraNodes`
  - 验证不足 `minimum_successful_writes` 时不调用 `CommitObject`
  - 验证失败路径仍等待全部已启动 replica task 完成
- `ObjectTransferWritePlanTest.UploadKeepsChunksSerialWhileReplicaFanoutRunsInParallel`
  - 验证 chunk 内 fan-out 并行时，不同 chunk 之间仍保持串行
- 既有 T004 authority/no-fallback/manifest correctness 测试继续保留并通过

## 验证命令和结果

```bash
cmake --build --preset debug-ninja-low-parallel
ctest --test-dir build/linux --output-on-failure -R "transfer_write_plan|storage_upload_integration|storage_upload_coordinator"
```

- 结果：PASS

## 状态

- PASS

## T005-B 必须继续处理的风险

- 当前仍等待全部已启动 replica task 完成；T005-B 需要在满足 quorum 后研究是否提前结束等待
- 当前不会主动取消已启动的慢副本 RPC；T005-B 需要明确 slow replica 的收尾策略
- `StorageTransferWriteRequest` 仍使用 request-owned payload string；若后续要进一步压缩单 chunk fan-out 的适配器层复制，需要单独设计并验证，不应在 T005-A 越界修改接口
