# T055 GC Metadata Safety

## 修改文件

- `modules/store/maintenance/garbage_collector.h`
- `modules/store/maintenance/garbage_collector.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_garbage_collector_test.cpp`
- `tests/storage_delete_gc_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t055-gc-metadata-safety.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `GarbageCollector` 新增必需的 `GarbageCollectorSafetyChecker` 注入点
- 为 safety gate 新增 `GarbageCollectorSafetyCheckResult`
- 在 worker loop 中把执行顺序收口为：
  - 先执行 metadata-driven safety checker
  - 仅当 checker 返回 `kOk` 时才调用 delete handler
  - checker 失败时直接更新 task 的 attempts / last_error / retryable / state
- 扩展 `storage_garbage_collector` 测试，覆盖：
  - live manifest violation 阻止删除
  - deleted/orphan/failed-upload task 通过 safety check
  - checker retryable unavailable -> retry -> 成功
  - repeated blocked task 不误删
- 扩展 `storage_delete_gc` 测试，使用真实 `MetadataStateMachine + LocalDiskChunkStore` 验证：
  - committed live manifest 仍引用 chunk 时，GC task 被拦截
  - deleted object chunk 通过 safety check 后可执行真实删除

## metadata-driven safety check 输入、输出和状态语义

- 输入：
  - 完整 `GarbageCollectorTask`
  - 至少包含 `chunk_id`
  - 保留 `reason`
  - 保留 `metadata_boundary`
- 输出：
  - `GarbageCollectorSafetyCheckResult`
  - `status == kOk`：允许执行 delete handler
  - retryable 状态如 `kNodeUnavailable` / `kTimeout`：metadata facts 临时不可用，进入 `RetryPending`
  - non-retryable 状态如 `kConflict`：视为 live manifest safety violation，任务进入 terminal `Failed`
- 状态语义：
  - `Queued -> Running -> Completed`
  - `Queued -> Running -> RetryPending -> Completed/Failed`
  - `Queued -> Running -> Failed`
  - safety failure 即使未调用 delete handler，也按一次完整 GC attempt 计数

## safety checker / delete handler / retry 当前边界

- safety checker 必须由调用方注入，当前不允许空 checker
- delete handler 仍必须由调用方注入，当前不允许空 handler
- live manifest 引用场景：
  - 不调用 delete handler
  - task 记录 `last_error` / `last_error_detail`
  - task 进入 `Failed`
- checker 临时不可用场景：
  - 映射为 retryable failure
  - 在 `attempts < max_attempts` 时允许重试
  - 当前仍是在同一个 worker 内立即重试
  - `next_retry_after_ms` 仅保留为扩展点，不实现延迟调度器
- current maintenance 边界：
  - maintenance 层不直接扫描 metadata
  - live-manifest 判断逻辑由外部 checker 决定

## 是否调用 metadata / Raft；是否真实删除 chunk

- `GarbageCollector` 生产代码本身不直接调用 metadata / Raft
- `GarbageCollector` 生产代码本身不直接实现 metadata 扫描
- 是否真实删除 chunk 仍由注入的 delete handler 决定
- 本次测试中：
  - `storage_garbage_collector` 使用 fake checker / fake handler
  - `storage_delete_gc` 使用真实 `MetadataStateMachine` fact checker 和真实 `LocalDiskChunkStore::DeleteChunk`

## 是否使用 tests/test_file/test_file.zip

- 否
- 本次新增 T055 测试使用 `MakeChunkPayload(...)`，未引入新的二进制 fixture

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "garbage_collector|storage_garbage_collector|storage_delete_gc|delete_gc" --output-on-failure 2>&1 | tee tmp/007/t055-gc-metadata-safety.log`
  - PASS
  - 日志路径：`tmp/007/t055-gc-metadata-safety.log`
  - 说明：实际匹配到的测试名为 `storage_delete_gc`、`storage_garbage_collector`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T055 是平台无关的 GC safety check / task-state 语义任务
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T055-WIN`

## 是否通过 T055

- 是

## 是否可以进入 T056

- 可以
- T056 应只继续补 pending timeout / failed upload / AbortObject cleanup candidate generation，不回头扩展 restart cleanup 或延迟调度器范围

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 `GarbageCollector` 已强制要求注入 safety checker，但 live-manifest 保护的正确性仍依赖调用方提供的 metadata 事实源是否新鲜且完整
- 当前没有 candidate generation；pending / failed upload / abort cleanup 候选仍留给 T056
- 当前没有 restart resume / cleanup persistence；仍留给 T057
- `next_retry_after_ms` 仍只是扩展点，没有真正的延迟重试调度器
- Windows 删除语义、timeout/cancellation 运行中传播、corruption 自动回写仍未解决
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
