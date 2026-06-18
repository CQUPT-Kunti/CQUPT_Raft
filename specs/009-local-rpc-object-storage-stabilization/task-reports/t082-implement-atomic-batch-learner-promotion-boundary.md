# T082 实施报告

## 结果

PASS

## 做了什么

- 在 `RaftNode` 内实现原子 batch learner promotion 边界。
- 将单个 `pending_add_learner_proposal_` 扩展为最多两个 learner 的 pending 集合。
- 在 leader 心跳路径上，当两个 pending learners 都达到 ready-to-promote 条件时，追加内部原子 batch promotion 日志命令，并仅在该命令 committed + applied 后一次性把 committed membership 从 `3 voters` 切换到 `5 voters`。
- 保持 single learner 不能形成 `4-voter committed membership`，并确保 learner 在 promote 前不参与 quorum / election。
- 调整相关测试以对齐新的 batch promote 边界和 restart 恢复行为。

## 修改文件

- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `tests/metadata_failover_test.cpp`
- `tests/test_raft_snapshot_restart.cpp`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 原子性与安全语义

- batch promote 只接受“一组 ready learners”进入目标配置，不走 unsafe single promote。
- committed membership 只在内部 batch promotion 命令 apply 时一次性更新到 `5 voters`，因此不会暴露或提交 `4-voter committed membership`。
- leader 失去领导权时会清理 pending learner runtime state 和 inflight batch promotion state，因此 failed / partial / interrupted promote 不会污染 committed membership。
- promote 完成后 committed membership、runtime membership、diagnostics 会同时反映 `5 voters / quorum 3`。
- 未实现 joint consensus，也没有通过两次 single promote 伪造 batch promote。

## 验证

构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum test_metadata_failover test_raft_snapshot_restart ) 9>/tmp/cqupt_raft_build.lock
```

关键子集：

```bash
ctest --preset debug-tests -R "(IntegratedObjectStorageQuorumTest\.(SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount|TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory|BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory)|MetadataFailoverTest\.LeaderFailureDuringIncompleteBatchPromoteDoesNotLeavePartialCommittedMembership|RaftSnapshotRestartTest\.RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership)" --output-on-failure
```

完整定向回归：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataFailover|Failover|RaftSnapshotRestart|SnapshotRestart" --output-on-failure
```

结果：

- 构建 PASS
- 关键子集 PASS
- 完整定向回归 PASS

## 残余风险

- 当前 batch promote 使用 `RaftNode` 内部日志命令表达原子 membership transition，已经满足 T082 的 committed/runtime safety 边界。
- 但 dedicated membership persistence / config trace / first-class history 仍不是独立通道，后续仍需由 T083 继续补强持久化与恢复边界。
