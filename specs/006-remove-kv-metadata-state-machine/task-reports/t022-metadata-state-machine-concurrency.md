# T022 MetadataStateMachine 并发安全最小边界

## 结论

- T022 已完成。
- `MetadataStateMachine` 已建立明确的最小线程安全边界。
- 本次未改 apply/query/snapshot 业务语义，只收敛锁策略并补并发测试。
- Linux 下已完成最小必要 configure / build / 定向 CTest 验证。

## 实际修改

- 更新 `modules/raft/state_machine/metadata_state_machine.h`
  - 将 `MetadataStateMachine::mu_` 从 `std::mutex` 升级为 `std::shared_mutex`。
- 更新 `modules/raft/state_machine/metadata_state_machine.cpp`
  - `Apply(...)` 改为写锁：`std::unique_lock<std::shared_mutex>`
  - `LoadSnapshot(...)` 最终状态替换路径改为写锁：`std::unique_lock<std::shared_mutex>`
  - `HeadObject / ListObjects / LastAppliedIndex / LastAppliedTerm / BucketCount / ObjectCount / RequestCount / TombstoneCount / FindBucket / FindObject / FindIndexedObjectId / FindChunkRefs` 改为读锁：`std::shared_lock<std::shared_mutex>`
  - `SaveSnapshot(...)` 改为先在读锁下复制一致性视图，再释放锁执行磁盘落盘，避免保存半更新状态。
  - `request_id` 去重、fingerprint 检查、业务状态变更、`last_applied_*` 更新仍保持在同一写锁临界区内。
- 更新 `tests/metadata_state_machine_test.cpp`
  - 新增 `ConcurrentDuplicateRequestIdApplyStaysIdempotent`
  - 新增 `ConcurrentHeadAndListReadsRemainConsistent`
  - 新增 `ConcurrentApplyAndQueryPreserveMetadataConsistency`
- 更新 `modules/raft/state_machine/AGENTS.md`
  - 同步 `MetadataStateMachine` 当前使用 shared/read 与 unique/write 的最小并发保护边界说明。

## 锁策略

- 写路径
  - `Apply(...)`：写锁
  - `LoadSnapshot(...)`：写锁
- 读路径
  - `HeadObject / ListObjects / Find* / Count / LastApplied*`：读锁
- Snapshot
  - `SaveSnapshot(...)`：读锁复制内存状态，随后无锁落盘
- 幂等边界
  - `request_id` 查重、fingerprint 比较、状态修改、索引更新、tombstone 更新、`last_applied_index / last_applied_term` 更新都在同一写锁内完成

## 并发测试覆盖

- 读路径
  - 多线程并发 `HeadObject / ListObjects / FindIndexedObjectId / FindChunkRefs`
  - 验证 committed object 读路径稳定，pending / deleted 不被错误暴露
- 重复 `request_id`
  - 多线程并发 replay 同一 `CreateObject` 请求
  - 验证只有一次真实 apply，其余返回 `idempotent replay`
- apply + query 一致性
  - 单写线程顺序执行 create/commit/delete/abort
  - 多读线程同时执行 `HeadObject / ListObjects / Find*`
  - 验证查询不会读到半更新状态，不破坏 `object_index / chunk_ref_index / tombstone` 一致性

## Linux 验证

- 选择原因
  - 本次修改了 `MetadataStateMachine` 头文件、实现和对应单测。
  - 按最小闭环执行 Linux configure + 受影响 target build + 对应测试过滤。
  - 未跑无关 target，未默认跑全量 CTest。

- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

- 结果
  - `cmake --preset debug-ninja-low-parallel`：PASS
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`：PASS
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`：PASS，`32/32` 通过

- 日志
  - configure 结果记录：`tmp/test-logs/t022-configure.log`
  - build 日志：`tmp/test-logs/t022-build.log`
  - ctest 日志：`tmp/test-logs/t022-ctest.log`

## 未跑全量 CTest 的说明

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 原因：本次只影响 `MetadataStateMachine` 的锁策略和其对应并发单测，不涉及 `RaftNode` 默认主路径、service、DataNode 或全局回归入口。

## 风险与边界

- 当前目标是工业化最小正确性保护，不是性能优化。
- `SaveSnapshot` 采用“读锁复制 + 锁外落盘”策略，保证视图一致，但不追求长时间 writer 友好度最优。
- 本次未把 `StrongConsistencyMetadataStateMachine` 一并升级为 `shared_mutex`，范围只限 `MetadataStateMachine`。

## 验收结果

- `MetadataStateMachine` 已有明确线程安全边界：已完成
- `Apply / LoadSnapshot` 使用写锁：已完成
- `Head/List/Find` 类接口使用读锁：已完成
- `SaveSnapshot` 可获得一致性视图：已完成
- `request_id` 幂等检查与状态变更在同一临界区：保持成立
- 并发测试覆盖读路径、重复 `request_id`、apply/query 基础一致性：已完成
- 未改变已有 apply/query/snapshot 语义：保持成立
- `MetadataStateMachine` 不依赖 KV：保持成立
- 未修改 `RaftNode` 默认 wiring：保持成立
- 未删除 KV：保持成立
- 未进入 T023：保持成立

## 说明

- `tasks.md` 当前已有另一条不同含义的 `T022`，本次按用户明确指令执行并单独出具报告，未改 `tasks.md` 标记。
