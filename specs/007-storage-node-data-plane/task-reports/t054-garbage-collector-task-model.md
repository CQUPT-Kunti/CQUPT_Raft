# T054 Garbage Collector Task Model

## 修改文件

- `modules/store/AGENTS.md`
- `modules/store/maintenance/AGENTS.md`
- `modules/store/maintenance/garbage_collector.h`
- `modules/store/maintenance/garbage_collector.cpp`
- `modules/store/maintenance/module-notes.md`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/storage_garbage_collector_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/task-reports/t054-garbage-collector-task-model.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `modules/store/maintenance` 模块及其 `AGENTS.md` / `module-notes.md`
- 实现最小生产 `GarbageCollector`
  - GC task model
  - bounded 后台队列
  - task state 转换
  - attempts / last_error / retryable / next_retry_after_ms 扩展点
  - submit / drain / stop / stats / task lookup
  - 注入式 delete handler
- 新增 `tests/storage_garbage_collector_test.cpp`
  - 覆盖合法/非法提交、队列满、成功执行、retryable failure 重试、non-retryable failure、max_attempts、drain、stop、cancel pending、metadata_boundary 保留
- 更新 CMake，把 `garbage_collector.cpp` 接入 `raft_core`，并注册 `storage_garbage_collector` 测试

## GarbageCollector task model 字段、状态和 bounded queue 语义

- task model 字段包含：
  - `task_id`
  - `chunk_id`
  - `object_id`
  - `version`
  - `chunk_index`
  - `reason`
  - `metadata_boundary`
  - `attempts`
  - `max_attempts`
  - `last_error`
  - `last_error_detail`
  - `state`
  - `retryable`
  - `next_retry_after_ms`
- `reason` 当前至少支持：
  - `DeletedObjectCleanup`
  - `OrphanChunkCleanup`
  - `FailedUploadCleanup`
  - `AbortCleanup`
- state 当前包含：
  - `Queued`
  - `Running`
  - `RetryPending`
  - `Completed`
  - `Failed`
  - `Cancelled`
- bounded queue 语义：
  - `queue_capacity` 有上限
  - 队列满时 `SubmitTask()` 返回 `Overloaded`
  - `worker_count == 0` / `queue_capacity == 0` 会被修正到安全最小值 `1`
  - `task_id` 重复返回 `AlreadyExists`
  - `Stop()` 后不再接受新 task

## handler / retry / attempts / last_error 当前边界

- 删除执行必须通过注入的 `delete handler` 间接完成；`GarbageCollector` 自身不硬编码直连 `LocalDiskChunkStore` 或 `StorageNodeClient`
- handler 成功：
  - task 进入 `Completed`
  - `attempts` 增加
- handler 失败：
  - 更新 `attempts`
  - 更新 `last_error`
  - 更新 `last_error_detail`
  - 更新 `next_retry_after_ms`
- retryable failure：
  - 只在 `attempts < max_attempts` 且 `Stop(kCancelPending)` 未阻止时继续
  - 当前在同一个 worker 内立即进入下一次 attempt
  - `next_retry_after_ms` 当前只作为扩展点和可观察事实，不实现延迟调度器
- non-retryable failure：
  - 直接进入 `Failed`
- 达到 `max_attempts`：
  - 进入 terminal `Failed`
- `Drain()`：
  - 等待当前已提交 task 进入 terminal state
  - 不停止 collector，自身仍可继续接收新 task
- `Stop(kDrain)`：
  - 停止接收新 task
  - 已排队和运行中的 task 继续走到 terminal state
- `Stop(kCancelPending)`：
  - 停止接收新 task
  - 尚未开始的 queued task 进入 `Cancelled`
  - 不中断已经开始运行的当前 attempt，也不再继续新的 retry

## 是否调用 metadata / Raft；是否真实删除 chunk

- 不调用 metadata / Raft
- `GarbageCollector` 本身不真实删除 chunk
- 是否真的删除由注入 handler 决定；本任务测试使用 fake handler 固定执行结果

## 是否使用 tests/test_file/test_file.zip

- 否
- T054 的 task model / bounded queue 测试不需要真实 payload fixture

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "garbage_collector|storage_garbage_collector" --output-on-failure 2>&1 | tee tmp/007/t054-garbage-collector.log`
  - PASS
  - 日志路径：`tmp/007/t054-garbage-collector.log`
  - 说明：实际匹配到的测试名为 `storage_garbage_collector`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证未失败

## Windows 验证判断

- T054 是平台无关 task model / bounded queue 任务
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T054-WIN`

## 是否通过 T054

- 是

## 是否可以进入 T055

- 可以
- 进入 T055 时应只补 metadata-driven safety check，不回头扩展 task model 或 candidate generation 范围

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 GC 只有 task model / queue / handler 注入边界，是否允许真实删除仍要等 T055 的 metadata-driven safety gate
- `next_retry_after_ms` 目前只是扩展点，没有真正的延迟重试调度器
- 当前没有 candidate generation、restart resume、Windows 删除语义验证、timeout/cancellation 运行中传播
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/maintenance/module-notes.md`
- 新增了 `modules/store/maintenance/AGENTS.md`
- 更新了 `modules/store/AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补充：
  - task validation helper
  - state / identity 收口 helper
  - bounded queue submit helper
  - worker loop helper
  - retry / attempts / last_error 更新边界
  - stop / drain helper
  - stats aggregation helper
