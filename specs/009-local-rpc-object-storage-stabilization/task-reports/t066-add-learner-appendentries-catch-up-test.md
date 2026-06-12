# T066 Add Learner AppendEntries Catch-Up Test

## Scope

本任务只补测试，不改生产代码，不实现 learner catch-up 运行时逻辑。

目标是把 learner AppendEntries catch-up 的最小行为先钉在测试里：

- learner-like 接收者可以通过 `AppendEntries` 追日志
- 追日志不等于成为 voter
- 追日志不等于进入 quorum
- 复制失败不影响现有 voter quorum
- 不提前实现 snapshot catch-up
- 不提前实现 promote-to-voter

## Files Changed

- `tests/test_raft_log_replication.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t066-add-learner-appendentries-catch-up-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Test Added

新增测试：

- `RaftLogReplicationTest.LearnerLikeAppendEntriesCatchUpDoesNotAffectCommittedVoterQuorum`

## Test Design

当前生产代码还没有完整 learner runtime membership，因此测试采用“3 voter 集群 + 1 个独立 learner-like 接收者”的方式表达 T066 边界：

1. 启动 3 voter Raft 集群，等待 leader。
2. 由 leader 提交 bucket create 和 object create 两条 metadata 日志。
3. 构造一个不在 committed voter set 中的独立 `RaftNode` 作为 learner-like 接收者。
4. 直接对它调用 `OnAppendEntries(...)`：
   - 第一次复制把日志追到 leader 的已知日志尾
   - 但只把 `leader_commit` 推到较早位置，证明“日志已到达”与“已提交/已应用”分离
5. 发送 duplicate `AppendEntries`，验证不会污染进度。
6. 发送一个带错误 `prev_log_index` 的 `AppendEntries`，制造 learner replication failure。
7. 在 learner 复制失败后，leader 继续对 3 voter 集群提交 `commit-object`，验证 voter quorum 不受影响。
8. 最后再对 learner-like 接收者发送后续 `AppendEntries`，把它追到最新日志并应用 committed 结果。

## What This Proves

这个测试当前证明了：

- learner-like 接收者可以通过 `AppendEntries` 接收和追加 leader 日志
- learner-like 接收者的 `last_log_index` / `commit_index` / `last_applied` 会推进
- duplicate / stale-style replication 不会污染 committed voter membership
- learner-like 接收者不进入 leader 的 committed voter set
- leader 的 committed quorum 仍然是 3 voters / quorum 2
- learner replication failure 不影响现有 voter majority 的后续提交
- catch-up 不会自动 promote-to-voter

当前没有在这个测试里直接证明 `RequestVote` / election 排除 learner；那部分按任务分工留给 `T068`。

## Validation

### Build

```bash
cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication
```

- PASS

### Test

```bash
ctest --preset debug-tests -R "RaftLogReplicationTest" --output-on-failure
```

- PASS
- 3/3 passed

## Result

- PASS
- 已在 `tasks.md` 只勾选 `T066`
- 可以进入后续任务

## Notes

- 本任务没有实现 learner catch-up 生产逻辑
- 本任务没有实现 snapshot catch-up
- 本任务没有实现 promote-to-voter
- 本任务没有修改 committed voter quorum 规则
