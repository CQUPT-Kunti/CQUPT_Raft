# T031 MetadataStateMachine shared_mutex 并发模型改造

## 修改文件

- `modules/raft/state_machine/metadata_state_machine.cpp`
- `tests/metadata_state_machine_concurrency_test.cpp`
- `tests/support/metadata_test_utils.h`
- `specs/006-remove-kv-metadata-state-machine/task-reports/T031-metadata-state-machine-shared-mutex.md`

## shared_mutex 改造点

- `MetadataStateMachine` 已继续沿用 `std::shared_mutex mu_` 作为 V2 状态保护锁。
- `Apply()` 使用 `std::unique_lock<std::shared_mutex>` 包住整次 metadata apply。
- `LoadSnapshot()` 使用 `std::unique_lock<std::shared_mutex>` 在一次提交里替换整套表。
- `SaveSnapshot()` 使用 `std::shared_lock<std::shared_mutex>` 复制一致性视图后再做文件写入。
- `HeadObject()`、`ListObjects()`、`LastAppliedIndex()`、`LastAppliedTerm()`、`Find*()` 查询路径使用 `std::shared_lock<std::shared_mutex>`。

## Apply 写锁范围

- 写锁从 request_id 幂等检查开始，一直持有到 bucket/object/index/chunk/request/tombstone 更新完成。
- 成功 apply 只在结构更新完成后推进 `last_applied_index_` / `last_applied_term_`。
- 失败命令在返回前不会推进 `last_applied_index_`。
- `LoadSnapshot()` 改为先替换 bucket/object/index/chunk/request/tombstone，再写入 `last_applied_index_` / `last_applied_term_`，避免内部提交顺序倒置。

## Head/List 读锁范围

- `HeadObject()` 在同一个 shared lock 视图内检查：
  - bucket 是否存在且未删除
  - object 是否存在且为 `COMMITTED`
  - `object_index` 是否可见
  - 可选 `object_id/version` 是否匹配
- `ListObjects()` 在同一个 shared lock 视图内遍历 `objects_`，并同时核对：
  - bucket 未删除
  - object 为 `COMMITTED`
  - `object_index` 存在
  - 没有 tombstone 残留
- 因为查询全程不释放 shared lock，所以不会读到 `object_table` 与 `object_index` 半更新状态。

## request/object/tombstone/object_index 原子更新说明

- `CreateObject` 仍在一次写锁内写入 `objects_`、`object_index_`、`requests_`、`request_fingerprints_`。
- 本次补了 `CreateObject` 对 stale tombstone 的清理：
  - 重新创建同名对象时会先清掉旧 `tombstones_`
  - 同时清掉可能遗留的 `chunk_ref_index_`
- `CommitObject` 现在在同一次 apply 内显式刷新：
  - `ObjectRecord.state = COMMITTED`
  - `object_index_[identity]`
  - `chunk_ref_index_[identity]`
  - `requests_[request_id]`
  - `request_fingerprints_[request_id]`
- `DeleteObject` 在同一次 apply 内原子完成：
  - `ObjectRecord.state = DELETED`
  - `object_index_.erase(identity)`
  - `chunk_ref_index_.erase(identity)`
  - `tombstones_[identity]`
  - `requests_[request_id]`
  - `request_fingerprints_[request_id]`

## 新增/调整测试

- 并发测试保留在 `tests/metadata_state_machine_concurrency_test.cpp`，未塞回 `metadata_state_machine_test.cpp`。
- 测试 suite 统一改成 `MetadataStateMachineConcurrencyTest`，便于按 T031 过滤执行。
- 保留并通过：
  - `ConcurrentDuplicateRequestIdApplyStaysIdempotent`
  - `ConcurrentHeadAndListReadsRemainConsistent`
  - `ConcurrentApplyAndQueryPreserveMetadataConsistency`
- 新增并通过：
  - `DeleteThenRecreateClearsStaleTombstoneAndKeepsVisibleIndexConsistent`
- `tests/support/metadata_test_utils.h` 新增 `ApplyMetadataCommand()`，把并发测试里的重复 serialize/apply helper 收敛到 support 层。

## Linux 结果

- `cmake --preset debug-ninja-low-parallel`
- 结果：PASS

- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- 结果：PASS

## Windows 结果

- Windows 未执行，原因是当前环境为 Linux；T031 的 Windows 覆盖将在 T035 统一验证

## CTest 结果

- 先按建议尝试：
  - `ctest --test-dir build/linux/tests --output-on-failure -R "MetadataStateMachineConcurrencyTest"`
  - 结果：`build/linux/tests` 下未注册 tests
- 实际可执行入口：
  - `ctest --test-dir build/linux --output-on-failure -R "MetadataStateMachineConcurrencyTest"`
  - 结果：PASS，4/4
- 日志：
  - `tmp/test-logs/t031-cmake-configure.log`
  - `tmp/test-logs/t031-build-test_metadata_state_machine.log`
  - `tmp/test-logs/t031-ctest-metadata-state-machine-concurrency.log`

## KV removal status

- 本任务未删除 KV 代码
- 本任务未修改 KV command / proto / service

## 是否修改 RaftNode / proto / CLI / scripts

- `RaftNode`：否
- `proto/raft.proto`：否
- `apps/raft_metadata_client.cpp`：否
- `test.sh / test.ps1`：否

## 是否进入 T032

- 否
- 本次只处理 `MetadataStateMachine` 内部 shared_mutex 并发模型与并发测试

## 剩余风险

- `last_applied_term_` 仍只能写成当前实现事实 `0`；真实 term 传播不在 T031 范围内。
- proposal admission / bounded queue / timeout / no-double-apply 的节点级并发防护仍属于 T032。
- 本次没有改 `metadata_state_machine_test.cpp` 的旧断言；如果后续需要把 `object_index` 语义从“内部对象入口”进一步收紧成“仅 committed 可见索引”，应在后续任务中连同对应单测一起调整。
