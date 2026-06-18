# T080 实施报告

## 结果

PASS

## 做了什么

- 在 `tests/metadata_failover_test.cpp` 中完成 leader failure during batch promote 测试。
- 测试现在可以进入真实 `2 ready learners` 边界，并验证 leader 在 batch promote 过程中失败后不会留下 partial committed membership，也不会形成 committed `4 voters`。

## 修改文件

- `tests/metadata_failover_test.cpp`

## 断言语义

- 初始 committed voter set 是 `3 voters`。
- 两个 learners 达到 ready-to-promote 前后都不参与 quorum / election。
- leader failure 后 committed membership 不能残留 partial state，不能掉到 committed `4 voters`。
- 新 leader 上 runtime membership、committed membership、retry/replay 结果保持一致，不会把 incomplete promote 当成 committed。
- learners 不会因为 failover 被错误恢复成 voters。

## 验证

已运行覆盖本任务的命令：

```bash
ctest --preset debug-tests -R "(IntegratedObjectStorageQuorumTest\.(SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount|TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory|BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory)|MetadataFailoverTest\.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership|RaftSnapshotRestartTest\.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership)" --output-on-failure
```

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataFailover|Failover|RaftSnapshotRestart|SnapshotRestart" --output-on-failure
```

结果：

- `MetadataFailoverTest.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership` PASS
- 完整定向回归 PASS

## 备注

- 当前 failover safety 已可验证真实 batch promote 边界。
- 更细粒度的 committed membership history / config trace 仍不是 first-class 诊断通道，残余风险已移入后续风险项。
