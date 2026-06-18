# T114 Confirm No Committed Voter Membership Test Allows Even Voter Count

## 1. 检查了哪些测试文件

- `tests/CMakeLists.txt`
- `tests/integrated_object_storage_quorum_test.cpp`

补充结论：

- 当前仓库中与本任务 target 对应的 integrated quorum 测试没有拆分到 `tests/integrated/*quorum*`、`*promote*`、`*membership*` 子文件。
- `tests/CMakeLists.txt` 中实际 target 为：
  - `test_integrated_object_storage_quorum`
  - 自定义别名 target `integrated_object_storage_quorum`

## 2. 静态扫描命中摘要

- 使用扫描命令：

```bash
grep -RInE "4-voter|4 voter|voters.*4|committed.*4|even|odd|quorum.*2|quorum.*3|ready learners|batch promote|single learner|waiting-for-pair|blocked" tests/ > /tmp/t114-even-voter-scan.txt || true
```

- 命中结果人工复核后，和本任务直接相关的核心命中集中在：
  - helper 防护：
    - `ExpectNoCommittedFourVoterDiagnostic(...)`
    - `ExpectNoCommittedFourVoterSummary(...)`
    - `ExpectNoObservableCommittedFourVoterHistory(...)`
  - single learner blocked / waiting-for-pair：
    - `SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`
    - `JoinMetadataClusterReportsPendingThenReadyLearnerWaitingForPair`
    - `SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal`
  - batch promote / no committed 4-voter history：
    - `TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
    - `BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`

## 3. 哪些测试覆盖 single learner blocked

- `IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`
  - 验证 observed learner 不会把 committed voter count 从 `3` 膨胀到 `4`
  - 验证停掉 1 个 committed voter 后，剩余 `2` 个 committed voters 仍可形成真实 quorum
  - 验证再停掉 1 个 voter 后，`1 voter + 1 learner` 不能满足 quorum
- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterReportsPendingThenReadyLearnerWaitingForPair`
  - 验证 single ready learner 返回 `promotion_status=waiting_for_pair`
  - 验证 `promotion_block_reason=even_voter_count`
  - 验证 `committed_quorum_size=2`
- `IntegratedObjectStorageQuorumTest.SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal`
  - 验证 direct promote 被拒绝
  - 验证拒绝后不追加 partial committed membership log
  - 验证 committed membership 仍保持 `3 voters / quorum=2`

## 4. 哪些测试覆盖 no committed 4-voter history

- `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
  - 验证 `3 voters + 2 ready learners` 直接提交到 `5 voters`
  - 验证 promote 返回 `committed_voter_count=5`
  - 验证 committed quorum 直接变成 `3`
  - 通过 `ExpectNoObservableCommittedFourVoterHistory(...)` 检查运行中节点 summary 与诊断消息都不出现 committed `4 voters`
- `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
  - 在 first learner ready、second learner join、explicit batch promote 各阶段都调用 `ExpectNoObservableCommittedFourVoterHistory(...)`
  - 验证整条 blocked / interrupted promote 路径不会留下 committed `4-voter` 历史

## 5. 哪些测试覆盖 3 voters + 2 learners -> 5 voters

- `IntegratedObjectStorageQuorumTest.TwoReadyLearnersMustBatchPromoteDirectlyToFiveCommittedVotersWithoutCommittedFourVoterHistory`
- `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`

这两条用例都显式断言：

- promote 后 committed voter ids 为 `5` 个
- `voter_count == 5`
- `committed_voter_quorum_size == 3`
- learner count 归零

## 6. 哪些测试覆盖 restart / failover / retry 不留下 even voters

- `retry / duplicate / replay`：
  - `IntegratedObjectStorageQuorumTest.DuplicateObservedJoinCandidateDoesNotCreateDuplicateCommittedMembershipEntry`
  - `IntegratedObjectStorageQuorumTest.JoinMetadataClusterReportsPendingThenReadyLearnerWaitingForPair`
  - `IntegratedObjectStorageQuorumTest.BlockedOrInterruptedBatchPromotePathNeverExposesCommittedFourVoterHistory`
- `failover-like voter loss / quorum boundary`：
  - `IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`
  - `IntegratedObjectStorageQuorumTest.ThreeVotersPlusObservedLearnerKeepsCommittedQuorumAtTwo`
  - `IntegratedObjectStorageQuorumTest.FiveVoterCommittedMembershipKeepsQuorumThreeAndAllowsCommitWithThreeReachableVoters`

说明：

- 当前 `integrated_object_storage_quorum_test.cpp` 中没有 dedicated restart 场景。
- 本次静态审计确认：即使没有 dedicated restart case，本 target 内也不存在把 committed `4 voters` 当作成功状态的断言、等待条件或 helper 入口。

## 7. 是否发现任何 even committed voter 成功路径

- 未发现。

人工复核结论：

- 没有测试把 `committed voter count = 4` 当作成功结果。
- 没有测试把 learner 计入 committed voter count。
- 没有测试把 `quorum=3` 当作 `3 voters + 1 learner` 的合法结果。
- 没有 helper 通过 `MakeCluster(4)` 或 `expected_voter_ids.size()==4` 的方式把 even committed voters 伪造成预期成功路径。

## 8. 是否新增或修正测试断言

- 有。
- 在 `tests/integrated_object_storage_quorum_test.cpp` 中新增了 3 处 helper 级防护：
  - `MakeCluster(...)` 禁止直接启动 committed `4-voter` cluster
  - `ExpectCommittedMembershipUnchangedOnRunningNodes(...)` 禁止把 `4-voter` committed membership 作为合法 expected state
  - `WaitForCommittedMembershipOnRunningNodes(...)` 禁止把 `4-voter` committed membership 作为合法 success condition

这些修改只强化测试边界，不修改生产代码和协议语义。

## 9. targeted build/test 命令和结果

- targeted build：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：PASS

- targeted test：

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure
```

- 结果：PASS
- `14/14` tests passed
- 日志：`tmp/test-logs/t114-integrated-quorum.log`

## 10. 最终状态

- PASS

## 11. 是否已勾选 T114

- 是。

## 12. 是否可以进入 T115

- 可以。
