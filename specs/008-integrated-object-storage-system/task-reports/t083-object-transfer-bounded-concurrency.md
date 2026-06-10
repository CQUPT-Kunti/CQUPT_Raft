# T083 - object transfer bounded concurrency controls

## 1. 修改了哪些文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`

本任务没有修改 `modules/store/transfer/object_transfer.h`。

## 2. upload/download sessions bounded concurrency controls 做了什么

- 在 `object_transfer.cpp` 内新增会话级 `SessionConcurrencyBudget`，显式记录：
  - `requested_concurrency`
  - `effective_concurrency`
  - `max_inflight_chunks`
  - `max_buffered_chunks`
  - `max_task_slots`
  - `max_inflight_payload_bytes`
- 对 upload/download session 都收紧为当前实现真实支持的单 chunk in-flight 路径。
- 将 `TransferSessionSnapshot.concurrency` 更新为实际生效的 `effective_concurrency`，避免把未实现的并发度伪装成真实能力。
- 在 upload/download 结果诊断里追加 bounded concurrency policy 说明，明确请求并发、实际并发和 payload/buffer 上界。

## 3. 如何限制 in-flight chunk、任务数、buffer 和内存占用

- 当前每个 session 的上限固定为：
  - `max_inflight_chunks = 1`
  - `max_buffered_chunks = 1`
  - `max_task_slots = 1`
- upload 使用请求的 `chunk_size` 估算 `max_inflight_payload_bytes`。
- download 在拿到 COMMITTED manifest 后，以 manifest 中最大 chunk size 估算 `max_inflight_payload_bytes`。
- 如果 CLI 传入更大的 `concurrency`，会被 clamp 到当前实现真实支持的上界，并写入诊断，防止 object transfer 层向无界队列、无界 async task 或整文件常驻内存路径漂移。

## 4. 如何保证大文件路径不整文件入内存

- 没有引入新的整文件缓存。
- upload 仍按现有 bounded chunk reader 逐 chunk 两遍读取。
- download 仍按 COMMITTED manifest 逐 chunk 读取、校验并写入临时文件。
- T083 只把每个 session 的并发资源预算显式化，没有放宽 payload boundary。

## 5. 如何处理并发失败、session cancellation、cleanup candidate、retry/backoff 之间的边界

- T083 没有改写现有失败语义，也没有新增无界等待或后台线程。
- upload 失败后的 cleanup candidate emission 仍沿用 T081 的已有路径；本任务只保证不会因更高并发参数放大成不可诊断的多 chunk 同时悬挂状态。
- StorageNode transient retry/backoff 仍由 T082 的 `StorageTransferClient` 负责；T083 不重复实现，也不掩盖 checksum/data corruption。
- session cancellation / IO / discovery / metadata / storage 失败仍沿用现有 `Fail(...)` 诊断边界。

## 6. 是否发现不合理点 / 警告 / 风险

- 当前实现的 bounded concurrency 是“显式收紧”而不是“真正多 chunk 并行”。
- 这满足了 T083 对有界资源控制的要求，但 100 operations 压测想体现更高吞吐时，仍需要后续在不破坏这些预算边界的前提下再扩展真正的并行 pipeline。
- 因此 T084 之后如果接入并发压测 target，应预期本任务首先保证安全边界和可诊断性，而不是吞吐最大化。

## 7. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 8. 验证命令和结果

### diff 检查

```bash
git diff -- modules/store/transfer/object_transfer.cpp modules/store/transfer/object_transfer.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t083-object-transfer-bounded-concurrency.md
```

结果：已执行，变更范围符合 T083 约束；`object_transfer.h` 无改动。

### 最小 build

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_client' \
|| echo "build lock busy, skip object_transfer bounded-concurrency build in this window"
```

结果：已执行，`storage_client` 构建成功。

### 相关测试

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "object_transfer|integrated_object_storage_concurrency|storage_transfer" --output-on-failure' \
|| echo "build/test lock busy, skip bounded-concurrency tests in this window"
```

结果：已执行，`ctest` 返回 `No tests were found!!!`。当前并发/recovery 相关测试 target 仍未由 T084 完成接线，因此本任务保留了 diff + 最小 build 结果，没有越权修改测试 target。
