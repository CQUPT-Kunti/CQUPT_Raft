# T079 实施报告

## 结果

PASS

## 做了什么

- 在 `tests/integrated_object_storage_quorum_test.cpp` 中完成 no committed `4-voter` history 断言。
- 测试覆盖 `3 voters + 2 ready learners -> direct 5 voters` 路径，并在可观察 committed membership / diagnostics 上持续检查不存在 committed `4 voters`。
- 相关生产前置缺口已由后续 T082 实现补齐，因此该测试现已可真实跑通，不再是红灯占位。

## 修改文件

- `tests/integrated_object_storage_quorum_test.cpp`

## 断言语义

- promote 前 learners 仍不是 voters，quorum 仍按 `3 voters / quorum 2` 计算。
- single learner 不能形成 committed `4-voter membership`。
- 两个 learners 都 ready 后，committed membership 直接变为 `5 voters`，quorum 变为 `3`。
- committed membership summary、runtime summary、JoinMetadataCluster diagnostics 中都看不到 committed `4 voters` 中间态。

## 验证

已运行覆盖本任务的命令：

```bash
ctest --preset debug-tests -R "(IntegratedObjectStorageQuorumTest\.(SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount|TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory|BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory)|MetadataFailoverTest\.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership|RaftSnapshotRestartTest\.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership)" --output-on-failure
```

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataFailover|Failover|RaftSnapshotRestart|SnapshotRestart" --output-on-failure
```

结果：

- `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory` PASS
- `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory` PASS
- 完整定向回归 PASS

## 备注

- 当前 no-committed-`4-voter` 断言仍主要依赖 committed quorum summary、runtime summary 和 diagnostics 观察面。
- first-class committed membership history trace 仍未单独落地，该残余问题已保留在 `cross-task-risk-notes.md` 的后续风险中。
