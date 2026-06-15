# T081 实施报告

## 结果

PASS

## 做了什么

- 在 `tests/test_raft_snapshot_restart.cpp` 中完成 committed batch membership restart recovery 测试。
- 测试现在可构造真实 committed `5-voter` batch membership，生成 snapshot / restart 后验证恢复仍是 `5 voters / quorum 3`，不会退回 `3 voters`，也不会恢复出 committed `4-voter` 中间态。

## 修改文件

- `tests/test_raft_snapshot_restart.cpp`

## 断言语义

- `3 voters + 2 ready learners` 通过安全 batch promote 达到 committed `5 voters`。
- restart / snapshot recovery 后 committed voter set 仍为 `5 voters`，quorum 为 `3`。
- promoted voters 不会退回 learners。
- recovery 不会把 partial / blocked promote 当成 committed membership。
- retry join 不会重复 apply promote，也不会制造 committed `4-voter` state。

## 验证

已运行覆盖本任务的命令：

```bash
ctest --preset debug-tests -R "(IntegratedObjectStorageQuorumTest\.(SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount|TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory|BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory)|MetadataFailoverTest\.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership|RaftSnapshotRestartTest\.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership)" --output-on-failure
```

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataFailover|Failover|RaftSnapshotRestart|SnapshotRestart" --output-on-failure
```

结果：

- `RaftSnapshotRestartTest.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership` PASS
- 完整定向回归 PASS

## 备注

- 当前 restart 恢复语义已覆盖 committed `5-voter` batch membership。
- dedicated membership persistence / history trace 仍需要后续任务继续补强，但不影响本任务的恢复语义验证成立。
