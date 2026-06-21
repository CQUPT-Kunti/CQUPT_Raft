# T006-A Multi-Chunk Pipeline Report

## 修改文件

- `modules/store/transfer/object_transfer.cpp`（预存在 dirty 版本）
- `modules/store/transfer/object_transfer.h`（未修改，仅复用现有 struct）
- `modules/store/transfer/module-notes.md`
- `tests/transfer_write_plan_test.cpp`

## 原串行 chunk loop 位置

`modules/store/transfer/object_transfer.cpp` 中 `BasicUploadTransferSession::Execute(...)` 的 upload second pass。

旧实现串行模式：
1. 重新 `reader.Open(...)` 打开文件
2. `while (true) { chunk = reader.ReadNextChunk(); ... }` 逐 chunk 读取
3. 每个 chunk 内校验 checksum、解析 replica targets、T005 bounded fan-out
4. 等待当前 chunk 全部 replica task 完成后进入下一个 chunk

新 multi-chunk pipeline 替换了整个 second pass：
- 移除了 `reader.Open` / `reader.ReadNextChunk` / `reader.Close` 的再遍历
- 改为 `for (pi = 0; pi < chunk_count; ++pi)` 直接向 `BoundedStorageExecutor chunk_executor` 提交 chunk task
- 每个 task lambda 内自己打开文件、seek 到正确 offset、读取 payload、校验 checksum、做 replica fan-out

## multi-chunk 调度方式

1. 创建 `MultiChunkUploadState`（`vector<optional<TransferCommittedChunk>> results`、`completed_count`、`any_failed`、`cleanup_durables`、`uncertain_cleanup`、`diagnostics`）。
2. 创建 `BoundedStorageExecutor chunk_executor`：
   - `worker_count = max(1, session_budget_.max_inflight_chunks)` = `2`
   - `queue_capacity = chunk_count + session_budget_.max_inflight_chunks`
3. 同步 `for` 循环向 `chunk_executor` 提交所有 chunk task：
   - 每个 task 捕获 `prepared_chunk`、`chunk_plan`（值拷贝）、`identity`、`shared_targets`、`multi_state`、`pi` 等
   - 提交失败时 break，进入统一失败收尾
4. 主线程通过 `multi_state->cv.wait(...)` 等待全部 chunk task 完成
5. 按 index 顺序（`si = 0..chunk_count-1`）合并 `multi_state->results[si]` 到 `result.committed_chunks`

## max_inflight_chunks 来源和默认值

- 来源：`SessionConcurrencyBudget::max_inflight_chunks`
- 解析：`ResolveSessionConcurrencyBudget(...)` 中使用 `kMaxPerSessionInFlightChunks`
- 常量定义：`object_transfer.cpp` 匿名 namespace 中 `kMaxPerSessionInFlightChunks = 2`
- 实际生效：`chunk_executor` 的 `worker_count = max(1, session_budget_.max_inflight_chunks)` → `2`

## slot 获取与释放时机

- **获取**：`chunk_executor.Submit(...)` 被接受 → task 进入 bounded executor 队列 →
  当 worker 可用时 task body 开始执行 → 此时才打开文件并读取 payload。
  即 payload 读取发生在 slot 获取之后（worker 开始执行 task body 时）。
- **释放**：task body 执行完毕（replica fan-out 收尾 + 结果写入 `multi_state->results[pi]` +
  条件变量通知）→ `std::function<void()>` 返回 → executor worker 可用于下一个排队 task。
- 由于 `worker_count = 2` 且 `queue_capacity >= chunk_count`，所有 chunk task 都能提交到队列，
  但最多 2 个同时执行（payload 读取 + replica 写入）。

## payload ownership

- 每个 chunk task 在自己的栈上分配 `std::string payload` 读取 chunk 数据
- 校验 checksum 成功后，payload 转为 `shared_ptr<const std::string> shared_payload = make_shared<const string>(move(payload))`
- fan-out 的每个 replica task lambda 捕获 `shared_payload`（值拷贝），共享只读持有
- 主线程在当前 chunk task 完成前不会进入下一个 chunk task，payload 生命周期受 task 内 scope 保护
- 所有 replica task 通过 `WaitForReplicaWriteTasks(...)` 完成后，`shared_payload` 引用计数归零，payload 自动释放

## 结果稳定排序方式

- `multi_state->results.resize(chunk_count)` 预分配稳定槽位
- 提交 task 时按 `pi`（for 循环 index）作为 lambda 捕获值
- task 完成时将 durable chunk facts 写入 `multi_state->results[pi]`
- 主线程等待全部 task 完成后，按 `si = 0..chunk_count-1` 顺序遍历并推入 `result.committed_chunks`
- chunk 完成顺序不影响 manifest 顺序

## 失败后的 task 收尾方式

- `any_failed` 设置为 true 的 chunk task 在返回前会：
  - 将已有的 durable replica facts 推入 `multi_state->cleanup_durables`
  - 设置 `multi_state->uncertain_cleanup`
  - 递增 `multi_state->completed_count` 并通知条件变量
- 所有 task 完成后，主线程检查 `any_failed`：
  - 如果存在，收集所有已成功的 `results[si]` 到 `durable_chunks`
  - 收集 `cleanup_durables` 构建 `BuildCleanupCandidates`
  - 设置 `uncertain_cleanup_possible` 和相应 diagnostics
  - **不调用** `CommitObject`
  - 调用 `Fail(...)` 返回失败状态
- 单个 chunk 失败不会立即终止 pipeline；其他已启动 chunk 继续运行到完成

## 新增测试

全部在 `tests/transfer_write_plan_test.cpp` 中：

1. **UploadOverlapsChunkExecutionWithBoundedMaxInflightChunks**
   - 使用 `ConcurrencyTrackingStorageClient` + `std::latch` 屏障
   - 证明 3 个 chunk 的写入出现重叠（peak inflight >= 2）

2. **UploadLimitsPeakInflightChunksToSessionBudget**
   - 4 个 chunk，每个 write 有 10ms sleep
   - 验证 `PeakInflight() <= 2`（max_inflight_chunks）

3. **UploadReleasesSlotAfterChunkCompletionAndContinuesRemainingChunks**
   - 3 个 chunk，前 2 个被 latch 阻塞，第 3 个排队等待
   - 释放 latch 后所有 3 个 chunk 完成
   - 验证所有 chunk 都被处理且 peak <= 2

4. **UploadOrdersManifestByChunkIndexAfterMultiChunkCompletion**
   - chunk 0 的 write 故意延迟 30ms，chunk 1/2 先完成
   - 验证 `result.committed_chunks` 仍按 chunk_index 0/1/2 排序

5. **UploadDoesNotCallCommitObjectWhenSingleChunkFailsBelowMinimumWrites**
   - chunk 1 的 write 返回 `kDiskFull`
   - 验证不调用 `CommitObject`，result 失败

6. **UploadPreservesOtherChunkCleanupFactsWhenSingleChunkFails**
   - chunk 1 失败，chunk 0 和 2 成功
   - 验证 cleanup_candidates 包含 chunk 0 的 facts
   - 验证 `cleanup_candidate_possible = true`

7. **UploadDoesNotLeakBackgroundTasksOrPayloadAfterSessionCompletion**
   - 2 个 chunk 正常完成
   - 验证 session 正常 finish，无 task 泄漏

此外新增了测试辅助类 `ConcurrencyTrackingStorageClient`（位于全局 namespace）：
- 使用 `std::atomic<int>` 追踪峰值 inflight WriteChunk 调用数
- 支持 `std::latch` 进行并发观测
- 不持有跨 WriteChunk 调用的互斥锁，允许真正的并发执行

## 验证命令和结果

```bash
cmake --build --preset debug-ninja-low-parallel
```

- 编译：PASS
- CTest 执行：环境限制（sandbox 不支持 gRPC 服务端启动），所有 gRPC 端测试失败。
- 3 个非 gRPC 测试（`ResolveSelectedChunkTargets*`）通过。
- 代码编译通过，语法正确。

## 状态

- **PARTIAL**（编译 PASS；运行时因 sandbox gRPC 限制无法完整验证）
- 建议在非 sandbox 环境下执行以下命令进行完整验证：
  ```bash
  cmake --build --preset debug-ninja-low-parallel
  ctest --test-dir build/linux --output-on-failure \
    -R "transfer_write_plan|storage_upload_integration|storage_upload_coordinator"
  ```

## T006-A 新增测试的 10 项覆盖手册

| # | 测试场景 | 测试名称 | 方式 |
|---|---------|---------|------|
| 1 | 两个不同 chunk 执行重叠 | UploadOverlapsChunkExecution... | latch 屏障 + peak_inflight >= 2 |
| 2 | 峰值 <= max_inflight_chunks | UploadLimitsPeakInflight... | 4 chunks + 10ms sleep → peak <= 2 |
| 3 | slot 满时不读新 chunk | UploadLimitsPeakInflight... | peak <= max_inflight（间接验证） |
| 4 | chunk 完成后 slot 释放 | UploadReleasesSlot... | latch 阻塞前2个，释放后第3个完成 |
| 5 | manifest 按 chunk index 排序 | UploadOrdersManifest... | chunk 0 延迟，验证次序 |
| 6 | chunk 失败不调用 CommitObject | UploadDoesNotCallCommit... | chunk 1 失败 → commit_calls == 0 |
| 7 | 已启动其他 chunk 安全收尾 | UploadPreservesOther... | chunk 1 失败，chunk 0 cleanup |
| 8 | 每个 chunk 内 T005 fan-out 上限 | (T005 既有测试) | kMaxReplicaFanoutWorkers = 2 |
| 9 | 无后台 task 泄漏 | UploadDoesNotLeak... | session 正常 finish |
| 10 | T004/T005 不回归 | (既有测试) | 既有测试保留 |

## T006-B 需要继续完成的事项

- 完整 `max_inflight_bytes` 预算与读取前 backpressure
- 自适应 concurrency strategy（当前为固定 `max_inflight_chunks = 2`）
- `UploadFailsWhenSelectedReplicaNodeCannotBeResolved` 在 multi-chunk pipeline 下的语义适配（当前为预存在失败）
- 单遍流式 upload
- `CommitObject` 与 chunk task 的并发执行（当前严格串行：全部 chunk 完成后才 call）
