# T092 Full Test Failure Summary

## 1. 执行命令

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'CTEST_PARALLEL_LEVEL=1 ./test.sh --group all'
```

## 2. 退出码

- 退出码：1

## 3. 失败测试列表或关键失败测试

- `MetadataFailoverTest.SameCommitRequestIdCanBeRetriedOnNewLeader`
- `MetadataConcurrencyStressTest.ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply`
- `IntegratedObjectStorageQuorumTest.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive`
- `storage_upload_coordinator`
  - 具体失败用例：`StorageUploadCoordinatorTest.ObjectChecksumSizeMismatchFailsBeforeMetadataCreateOrChunkWrite`

补充状态：

- CTest 汇总：`99% tests passed, 4 tests failed out of 272`
- disabled / not run：
  - `IntegratedObjectStorageE2ETest.AppConfigParsingSmokeCliOverridesMustRespectDurableIdentityAndStartupContracts`
  - `IntegratedObjectStorageE2ETest.HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage`
  - `IntegratedObjectStorageE2ETest.ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile`
  - `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256`

## 4. 关键错误摘要

### 4.1 Metadata failover 语义失败

- 关键片段：
  - `lost leadership before the log entry reached a majority`
- 表现：
  - `MetadataFailoverTest.SameCommitRequestIdCanBeRetriedOnNewLeader` 期望重试后成功提交，但当前返回了 leader 丢失 / majority 未达成的失败诊断。

### 4.2 Metadata concurrency 端口冲突 / 子进程异常

- 关键片段：
  - `Subprocess aborted`
  - `Error in bind for address '[::ffff:127.0.0.1]:48760': Address already in use`
  - `terminate called without an active exception`
- 表现：
  - `MetadataConcurrencyStressTest.ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply` 在启动测试节点时发生 gRPC bind 冲突。

### 4.3 Integrated object storage quorum 语义失败

- 关键片段：
  - `Actual: false`
  - `Expected: true`
  - `single surviving node illegally committed or applied new object after quorum loss`
- 表现：
  - `IntegratedObjectStorageQuorumTest.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive` 发现单存活节点在丢失 quorum 后仍然提交或应用了新对象。

### 4.4 Storage upload coordinator 断言失败

- 关键片段：
  - `result.error_detail`
  - `Which is: "object checksum size must match object_checksum.size"`
  - `Expected: "object_checksum.size must match summed chunk payload size"`
- 表现：
  - `StorageUploadCoordinatorTest.ObjectChecksumSizeMismatchFailsBeforeMetadataCreateOrChunkWrite` 的错误文案与测试期望不一致。

## 5. 涉及文件 / target / test suite

- 测试入口：
  - `test.sh`
- 失败测试 / 目标：
  - `tests/metadata_failover_test.cpp`
  - `tests/metadata_concurrency_stress_test.cpp`
  - `tests/integrated_object_storage_quorum_test.cpp`
  - `tests/storage_upload_coordinator_test.cpp`
  - `tests/storage_upload_coordinator` target
- 运行日志：
  - `tmp/test-logs/t092-full-test.log`
  - 脚本内 CTest 日志：`tmp/test-logs/t051-linux-full-ctest-single-worker.log`
  - 脚本内 failed tests 文件：`tmp/test-logs/t051-linux-failed-tests.md`

## 6. 初步失败分类

- `MetadataFailoverTest.SameCommitRequestIdCanBeRetriedOnNewLeader`
  - 分类：integration test failure / failover semantics regression
- `MetadataConcurrencyStressTest.ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply`
  - 分类：port conflict / flaky timing / test environment isolation issue
- `IntegratedObjectStorageQuorumTest.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive`
  - 分类：quorum safety failure / integration test failure
- `StorageUploadCoordinatorTest.ObjectChecksumSizeMismatchFailsBeforeMetadataCreateOrChunkWrite`
  - 分类：unit/integration assertion mismatch / diagnostic string drift

## 7. 是否做了最小修复；如未修复，说明原因

- 做了 1 处最小修复，然后重新运行 full test：
  - 修改 `test.sh`，增加 `--group all` 到默认全量流程的入口兼容。
- 该修复只属于测试入口兼容修复，不涉及业务语义、测试断言放宽或协议修改。
- 对上述 4 个真实失败，没有继续扩大修复范围；因为它们已经超出 T092 的“验证入口级小问题”边界。

## 8. 建议后续修复入口

- `MetadataFailoverTest.SameCommitRequestIdCanBeRetriedOnNewLeader`
  - 重点检查 `modules/raft/service/metadata_service_impl.cpp`、`modules/raft/node/raft_node.cpp` 与相关 failover / retry 语义，确认同一 request_id 在 leader 切换时的 propose / retry 判定是否回归。
- `MetadataConcurrencyStressTest.ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply`
  - 重点检查测试端口分配策略和 fixture 资源清理；优先看 `tests/metadata_concurrency_stress_test.cpp` 是否存在固定端口或端口复用窗口。
- `IntegratedObjectStorageQuorumTest.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive`
  - 重点检查 committed membership / quorum 判断链路，确认单节点存活场景下是否错误放宽了 commit/apply 条件。
- `StorageUploadCoordinatorTest.ObjectChecksumSizeMismatchFailsBeforeMetadataCreateOrChunkWrite`
  - 重点检查 `modules/store/upload/` 相关错误文案或测试期望，统一诊断字符串来源，避免 message drift。

## 9. T092 是否失败

- 失败。
