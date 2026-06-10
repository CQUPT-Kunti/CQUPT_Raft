# T082 任务报告

## 1. 修改了哪些文件

- `modules/store/transfer/storage_transfer_client.cpp`
- `modules/store/transfer/storage_transfer_client.h`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`

未修改：

- `common-risk-notes.md`
- `specs/008-integrated-object-storage-system/risk-register.md`

## 2. StorageTransferClient retry/backoff policy 做了什么

- 在 `StorageTransferClient` 层为 `WriteChunk` 和 `ReadChunk` 增加了有界 transient retry/backoff。
- 保留 `StorageNodeClientConfig::max_write_retries` 作为下层单次 `WriteChunk` RPC 内部重试上限；T082 新增的是 transfer adapter 自身的额外重试边界。
- 新增最小配置项：
  - `max_transient_write_retries`
  - `max_transient_read_retries`
  - `initial_backoff_ms`
  - `max_backoff_ms`
- retry 时复用同一 `request_id` 和 chunk identity，不改变幂等边界。
- 每次重试都会把 transfer 总 deadline 折算成“本次尝试剩余超时”，避免多次重试把总耗时放大成无界累计阻塞。
- backoff 使用有界指数退避；若服务端返回 `retry_after_ms`，只在本地 `max_backoff_ms` 上限内遵守。
- 重试耗尽或 deadline 到期时，会返回带 `request_id`、`chunk_id`、`node_id`、`endpoint`、`attempt`、`status` 的清晰诊断。

## 3. 如何区分可重试和不可重试 StorageNode 失败

依赖现有 `IsRetriableStatus(...)` 分类，只对以下临时失败重试：

- `Timeout`
- `IoError`
- `Overloaded`
- `NodeUnavailable`

以下错误不会被盲目重试：

- `ChecksumMismatch`
- `Corrupted`
- `InvalidArgument`
- `DiskFull`
- `NotFound`
- `Conflict`
- `PermissionDenied`
- `Cancelled`
- `Unsupported`

因此不会把 checksum/data corruption、payload/identity 参数错误、容量耗尽或对象可见性边界错误伪装成“多试几次也许会好”。

## 4. 如何避免无界重试、长时间阻塞和掩盖 checksum/data corruption

- retry 次数由 `max_transient_write_retries` / `max_transient_read_retries` 明确限制。
- 单次 sleep 由 `max_backoff_ms` 明确限制，默认值较小。
- 若请求设置了 `timeout_ms`，adapter 会基于绝对 deadline 计算每次尝试的剩余超时；deadline 到期立即停止，不继续 sleep 或 RPC。
- 非 transient 错误直接返回，不进入 retry。
- checksum mismatch / corruption 等完整性错误不会被 retry 覆盖。
- 本任务没有实现 T083 的全局 bounded concurrency，也没有修改 StorageNode 服务端 durability / checksum / recovery 语义。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前下层 `StorageNodeClient` 只对写路径提供内部重试，且没有 backoff；T082 在 transfer 层补了更外层的有界策略，但写路径现在存在“下层无退避重试 + 上层有退避重试”的双层结构。虽然边界仍然是有界的，但后续如果要细化时延模型，建议统一两层职责。
- 当前仓库里没有命中 `storage_transfer|object_transfer|integrated_object_storage_concurrency|integrated_object_storage_recovery` 正则的 CTest 用例；T082 目前完成了编译级验证，针对 retry/backoff 的专门测试仍需后续 target / 测试接入支撑。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

## 7. 验证命令和结果

### diff 检查

命令：

```bash
git diff -- modules/store/transfer/storage_transfer_client.cpp modules/store/transfer/storage_transfer_client.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t082-storage-transfer-retry-backoff.md
```

结果：

- PASS
- T082 相关文件 diff 正常。
- `tasks.md` 同一 hunk 中还带有既有的 `T080` 状态变更；该项不属于本次 T082 实现内容，本任务未回退该已有工作树变更。

### 最小相关 build

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_client'
```

结果：

- PASS
- 成功编译 `modules/store/transfer/storage_transfer_client.cpp`
- 成功链接 `raft_core` 与 `storage_client`

### 相关测试筛选

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "storage_transfer|object_transfer|integrated_object_storage_concurrency|integrated_object_storage_recovery" --output-on-failure'
```

结果：

- 当前返回 `No tests were found!!!`
- 说明当前没有命中该正则的已接入测试；本窗口未获得 T082 专项测试结果
- 未为此主动修改 CMake target，符合 “T084 负责 wire recovery/concurrency test targets” 的任务边界

## 结论

- T082 已完成实现与最小验证。
- 可进入后续 US6 任务，尤其是：
  - `T083` bounded concurrency controls
  - `T084` recovery/concurrency test target 接入
