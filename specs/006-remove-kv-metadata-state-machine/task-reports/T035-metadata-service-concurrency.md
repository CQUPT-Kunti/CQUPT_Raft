## T035 MetadataService 并发与 admission 测试增强

### 本轮增强内容
- `tests/metadata_concurrency_stress_test.cpp`
  - 强化 `AdmissionRejectsWhenInflightLimitIsReached`
    - overload 后额外验证 `request_table` 没被污染
    - 验证 `ListObjects` 为空，`object_table` 没有溢出对象
    - 验证 `last_applied_index` 仍停留在已提交 bucket 边界
  - 强化 `ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply`
    - 不只看同 `log_index`
    - 追加校验 `request_table`、`object_index`、`chunk_ref_index`、`Head/List`、`last_applied_index`
  - 新增 `ConcurrentDuplicateDeleteRequestsShareOneLogEntryAndKeepDeletionFactsConsistent`
    - 并发重复 `DeleteObject`
    - 断言只对应一个 `log_index`
    - 断言 `tombstone` 存在、`object_index` 清除、`chunk_ref_index` 清除、`Head/List` 不暴露

- `tests/metadata_failover_test.cpp`
  - 强化 `LeaderWriteTimeoutReturnsTimeoutAndSameRequestIdCanRetry`
    - timeout 后重试成功时，补验 `request_table`、`object_index`、`chunk_ref_index`、`Head/List`、`last_applied_index`
    - 调整用例内部 leader 重新确认，避免测试拿到过期 stub 造成假失败
  - 强化 `ConcurrentDuplicateCreateObjectRequestsShareSameLogIndex`
    - 并发重复 create 之后补做 commit
    - 断言可见对象只有一份，且 `request_table/object_index/chunk_ref_index` 一致
  - 新增 `ConcurrentConflictingCreateObjectRequestsReturnConflictAndKeepCommittedStateConsistent`
    - 同 `request_id` 并发提交不同 payload
    - 断言一个成功、一个 `IDEMPOTENCY_CONFLICT`
    - 断言最终 committed object、`Head/List`、`request_table/object_index/chunk_ref_index` 保持一致

### 复用的现有覆盖
- `MetadataFailoverTest.FollowerWriteReturnsNotLeader`
- `MetadataFailoverTest.FollowerHeadAndListReturnNotLeader`
- `MetadataConcurrencyStressTest.TimeoutReturnsWithoutBlockingAndRetryUsesSameInflightProposal`
- `MetadataClientScenarioTest.ClientShowsRetryableAdmissionStatuses`
- `MetadataClientScenarioTest.ReadCommandsShowRetryableAdmissionStatuses`

这些现有用例继续覆盖：
- follower 写 / Head / List 返回 `NOT_LEADER`
- timeout 返回明确错误
- stopped / `SERVICE_UNAVAILABLE` 的 CLI 展示

### Linux 验证
- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_concurrency_stress test_metadata_failover test_metadata_client_scenario`：PASS
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "(MetadataConcurrencyStressTest|MetadataFailoverTest|MetadataClientScenarioTest)"`：PASS
- CTest 结果：`21/21` 通过

### 风险
- stopped 节点的“真实网络请求命中 service admission”仍受 gRPC 传输层生命周期影响；节点完全停掉后，调用方可能先看到 transport failure，而不是 service 内部 `SERVICE_UNAVAILABLE`
- 当前读路径仍是 leader-local read；本轮只增强并发与 admission 测试，没有实现 `ReadIndex` / leader lease
- 本轮未改业务语义，未进入 T036
