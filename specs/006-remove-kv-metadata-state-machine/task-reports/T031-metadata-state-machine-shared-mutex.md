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


## T031 追加注意：delete 后同名重建与旧 lifecycle 防护

- 本次 `CreateObject` 会在重新创建同名对象时清理旧 `tombstones_`，该行为用于支持对象被 `DeleteObject` 后使用新的 `request_id` 开启新的同名 object lifecycle。
- 该行为可以接受，但必须以“旧 request_id / 旧 lifecycle 不能复活或污染新对象”为前提。
- 后续任务需要确认系统仍保留足够的旧生命周期事实，例如 `request_table`、`request_fingerprints_`、`object_epoch` / 等价 lifecycle 标识，或其他能够区分旧对象生命周期与新对象生命周期的机制。
- 正确语义应为：
  - `delete old object -> recreate same bucket/object with new request_id -> commit new object` 后，新对象应保持可见。
  - 旧 `CreateObject` / `CommitObject` / `DeleteObject` 的 `request_id` 重试只能返回幂等重放或冲突结果，不能重新 apply。
  - 旧请求不能覆盖新对象的 manifest、checksum、chunk refs、object state 或 visible index。
  - 清理旧 tombstone 不等于删除旧 request/lifecycle 的去重与冲突事实。
- 建议在后续 US3 / US4 测试中补充反向用例：
  1. `CreateObject(c1)` -> `CommitObject(m1)` -> `DeleteObject(d1)`；
  2. `CreateObject(c2)` -> `CommitObject(m2)` 重建同名对象；
  3. 再次重放旧 `c1` / `m1` / `d1`；
  4. 断言新对象仍可见且 metadata 未被旧请求覆盖，旧请求返回 replay/conflict，而不是形成新的成功 apply。
- 风险归属：这不是 T031 阻塞项，但必须在后续 T032/T035 或恢复相关 T043 前被测试锁定，避免 delete/recreate 场景在 leader switch、restart recovery 或 follower catch-up 后出现 stale retry resurrect / stale retry overwrite 问题。

## 剩余风险

- `CreateObject` 清理旧 tombstone 后，必须在后续任务中继续验证旧 `request_id` / 旧 lifecycle 重放不会复活或污染同名新对象；详见上一节追加注意。
- `last_applied_term_` 仍只能写成当前实现事实 `0`；真实 term 传播不在 T031 范围内。
- proposal admission / bounded queue / timeout / no-double-apply 的节点级并发防护仍属于 T032。
- 本次没有改 `metadata_state_machine_test.cpp` 的旧断言；如果后续需要把 `object_index` 语义从“内部对象入口”进一步收紧成“仅 committed 可见索引”，应在后续任务中连同对应单测一起调整。
