# T067 Add Learner InstallSnapshot Catch-Up Test

## Scope

本任务是 US4 learner snapshot catch-up 的测试先行任务。

- 只新增/更新测试。
- 不修改生产代码。
- 不实现 learner InstallSnapshot catch-up、AppendEntries catch-up、promote-to-voter 或 committed membership change。

## Files Changed

- `tests/test_raft_snapshot_catchup.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t067-add-learner-installsnapshot-catch-up-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Added Tests

- `RaftSnapshotCatchupTest.LearnerLikeReceiverInstallsSnapshotWithoutChangingCommittedVoterQuorum`
- `RaftSnapshotCatchupTest.FailedLearnerLikeInstallSnapshotDoesNotPolluteCommittedVoterMembership`

## Test Design

当前仓库还没有 runtime learner membership representation、learner election exclusion 和 learner quorum accounting，因此本任务没有伪造“真实 committed learner 已经存在”的生产前提。

本任务把目标收敛为一个 `learner-like snapshot receiver` 边界测试：

- 先让真实 3-voter 集群写入足够多的数据并生成 leader snapshot。
- 再构造一个不属于 committed voter set 的独立接收端节点。
- 直接向它发送 `InstallSnapshot` 请求。
- 验证 snapshot 安装成功时：
  - `InstallSnapshotResponse.success=true`
  - `last_snapshot_index` / `last_applied` / `MetadataStateMachine::LastAppliedIndex()` 推进到 snapshot 边界或以上
  - 真实 3-voter leader 的 `CommittedMembershipQuorumSummary` 保持 `voter_ids=[1,2,3]`、`voter_count=3`、`quorum_size=2`、`learner_count=0`
  - cluster 在安装后仍能正常选出 leader，说明 election authority 仍只在 committed 3-voter 集合内
- 验证 snapshot 安装失败时：
  - 损坏 snapshot 数据返回失败
  - 接收端 `last_snapshot_index` / `last_applied` 保持 `0`
  - leader 侧 committed voter set / quorum summary 不变

## Coverage Summary

- 覆盖了 learner 落后过多后通过 `InstallSnapshot` 接收 snapshot 的成功路径。
- 覆盖了 learner snapshot install 失败不污染 current membership 的失败路径。
- 覆盖了 snapshot catch-up 完成不等于 voter、不等于 promote、不改变 committed voter set 的边界。
- 覆盖了 learner snapshot receiver 不进入 quorum 的 leader-side 只读诊断边界。
- 覆盖了 learner snapshot receiver 不改变 election authority 的 cluster-side 边界。

## Not Implemented

- 未实现生产 learner InstallSnapshot catch-up 逻辑。
- 未实现生产 learner AppendEntries catch-up。
- 未实现生产 promote-to-voter。
- 未实现 committed learner membership change。
- 未修改 quorum / election 生产逻辑。

## Linux Validation

- Build command:

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_raft_snapshot_catchup
) 9>/tmp/cqupt_raft_build.lock > tmp/test-logs/t067-build.log 2>&1
```

- Build result: PASS
- Build log: `tmp/test-logs/t067-build.log`

- Test command:

```bash
ctest --preset debug-tests -R '^RaftSnapshotCatchupTest\.' --output-on-failure > tmp/test-logs/t067-ctest.log 2>&1
```

- Result: PASS
- Coverage: `6/6`
- Total time: `172.10 sec`
- Test log: `tmp/test-logs/t067-ctest.log`

## Result

- PASS
- 已在 `tasks.md` 中只勾选 T067
- 可以进入后续任务
