## T049

### 做了什么

- 扩展了 StorageNode heartbeat / report 的 proto、service 和 client 映射，把 `incarnation_id`、`sequence`、容量、负载、磁盘压力、健康状态、显式 `writable` 语义贯通到 registry 更新链路。
- 为了让这些语义能被 registry 持久保留和排序保护，最小同步了 `StorageNodeRegistry` 的 health facts 与 heartbeat merge。
- 更新了 `tests/storage_heartbeat_registry_test.cpp`，补充 `writable + incarnation` 相关断言和旧 incarnation 拒绝覆盖场景。

### 修改文件

- `proto/storage_node.proto`
- `modules/store/node/storage_node_client.h`
- `modules/store/node/storage_node_client.cpp`
- `modules/store/node/storage_node_service.cpp`
- `modules/store/node/storage_node_registry.h`
- `modules/store/node/storage_node_registry.cpp`
- `tests/storage_heartbeat_registry_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

### StorageNode heartbeat 现在能表达的运行时语义

- 长期 `node_id`
- 当前进程 `incarnation_id`
- 同一 incarnation 内递增 `sequence`
- `observed_at_unix_ms`
- 容量事实：`total/used/available/chunk_count`
- 健康事实：`health`、`disk_pressure`、`io_error_count`
- 显式 `writable` 状态
- 负载事实：`active_reads/active_writes/queued_ops`
- admission overload 状态：`write_admission_overloaded`、`read_admission_overloaded`
- failure domain：`zone/rack`

### incarnation / sequence 边界如何保持

- heartbeat / partial report 请求现在都会携带 `incarnation_id`，不会再只靠 `observed_at` 或 `sequence` 做新旧判断。
- registry 继续使用已有 incarnation-aware merge：
  - 更高 `incarnation_id` 优先
  - 同一 `incarnation_id` 内更高 `sequence` 优先
  - 旧 incarnation 或低 sequence 的 heartbeat 不会覆盖当前状态
- `writable` 会随 heartbeat / health report 进入 observed state；若请求显式不可写，或状态为 `READ_ONLY` / `UNAVAILABLE` / `FULL disk pressure`，registry 不会保留为可写。

### 新增或更新的测试

- 更新既有 heartbeat 断言，验证 `writable=false` 可被 registry 观察到。
- 更新 partial health merge 断言，验证 health report 可更新 `writable`。
- 更新 restart same node_id / new incarnation 测试，验证新旧 incarnation 切换后 `writable` 与当前状态一致。
- 新增：
  - `StorageHeartbeatRegistryTest.HealthWritableStateTracksHeartbeatAndRejectsConflictingOlderIncarnation`

### 验证命令和结果

- 构建命令：
  - `(
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target test_storage_heartbeat_registry
    ) 9>/tmp/cqupt_raft_build.lock`
  - 结果：PASS
- 测试命令：
  - `ctest --preset debug-tests -R "^storage_heartbeat_registry$" --output-on-failure`
  - 结果：PASS
- 测试日志：
  - `tmp/test-logs/t049-ctest.log`

### 结果

- 状态：PASS
- 已在 `tasks.md` 中只勾选 T049 完成。
- 可以进入 T050；当前 heartbeat 只补齐 observed-state 事实承载，不触碰 placement、旧对象 rebalance、Raft quorum 或 membership authority。
