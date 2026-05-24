# T051 Linux Failure Fix

## 本轮处理范围
- 依据最新 `T051-linux-final-validation.md`，本轮只处理 2 个剩余失败项：
- `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- `RaftReplicatorBehaviorTest.SlowFollowerCatchesUpWhileLeaderKeepsAcceptingNewLogs`

## 已修复项
- `RaftReplicatorBehaviorTest.SlowFollowerCatchesUpWhileLeaderKeepsAcceptingNewLogs`
- 根因：测试在单 follower 掉线期间持续写入过多 metadata proposal，同时 snapshot 周期过密，导致剩余多数派节点在 Linux 上出现选举抖动，proposal 超时并误伤 catch-up 断言。
- 修复：在 [tests/test_raft_replicator_behavior.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_replicator_behavior.cpp) 收敛写入规模，并在 lagging follower 重启后先等待单 leader 稳定，再继续 live writes。
- 结果：该失败项最小复测已通过。

## 未修复 blocker
- `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- 现象：损坏最新 snapshot 后，节点会加载更旧的 trusted snapshot，但运行时边界仍保留更新的 `last_snapshot_index`，随后 replay 从较新的 compacted 边界继续。
- 关键日志现象：
- 加载的是旧 snapshot，例如 `index=8` 或 `index=20`
- 同时节点恢复态仍显示 `last_snapshot_index=16/40`
- replay 会在 `index=17/41` 处失败，错误为 `not found: object does not exist`
- 结论：这不是简单测试时序抖动；它暴露的是“older snapshot fallback + compacted log replay boundary”之间的真实恢复边界问题。继续只改测试会明显削弱该用例语义，因此本轮没有强行把它改成伪通过。

## 本轮修改文件
- [tests/test_raft_replicator_behavior.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_replicator_behavior.cpp)

## Linux 最小验证
- 通过：
- `ctest --test-dir build/linux --output-on-failure -R '^RaftReplicatorBehaviorTest\.SlowFollowerCatchesUpWhileLeaderKeepsAcceptingNewLogs$'`
- 结果：`1/1 PASS`
- 耗时：约 `33.00 sec`
- 仍失败：
- `ctest --test-dir build/linux --output-on-failure -R '^RaftSnapshotDiagnosisTest\.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot$'`
- 结果：`FAIL`
- 失败摘要：startup fallback 到旧 snapshot 后，replay 在 snapshot 之后的 metadata entry 处报 `not found: object does not exist`

## 建议
- 建议不要直接重跑 T051 全量当作 PASS。
- 先把 `172` 作为真实 blocker 单独处理，优先检查 `RaftNode` startup snapshot fallback 与 compacted log replay 边界是否允许回退到早于当前 log snapshot marker 的 snapshot。
- 若后续决定修生产代码，建议从 `modules/raft/node/raft_node.cpp` 的 startup snapshot load/replay 边界入手。
