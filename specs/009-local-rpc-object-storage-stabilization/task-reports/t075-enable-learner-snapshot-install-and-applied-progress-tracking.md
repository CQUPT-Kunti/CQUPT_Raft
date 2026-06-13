# T075 Enable learner snapshot install and applied progress tracking

## Scope

- 任务类型：实现
- 本任务实现 learner 的 `InstallSnapshot` catch-up 与 applied/snapshot progress 记录。
- 本任务不实现 promote-to-voter、batch promote、joint consensus，也不修改 committed voter membership / quorum / election authority。

## Task Source

- `tasks.md`: T075
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `tests/test_raft_snapshot_catchup.cpp`
- `tests/test_raft_snapshot_restart.cpp`

## Files Changed

- `modules/raft/replication/replicator.h`
- `modules/raft/replication/replicator.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/test_raft_snapshot_catchup.cpp`

## What Changed

- 去掉了 learner 在 `Replicator` snapshot 分支上的直接拒绝，使 pending learner 在日志缺口越过 retained log 边界时可以进入 `InstallSnapshot` 路径。
- 在 `Replicator` 中补充 snapshot 诊断状态，记录最近一次 snapshot install 的 index/term、observed applied progress、peer last log progress，以及 success/failure 计数。
- 在 `RaftNode` 中补充 per-peer snapshot progress 跟踪，并把 `last_snapshot_index`、`last_snapshot_term`、`last_applied_index`、`observed_last_log_index` 暴露到 runtime membership summary，供 learner catch-up 诊断使用。
- 新增 pending learner snapshot catch-up 集成测试，验证 leader 通过 `ProposeAddLearner` 接入 learner 后，能经由 snapshot 安装推进 learner progress，同时 committed voter quorum 不变化。

## Boundary Checks

- 没有实现 promote-to-voter。
- 没有实现 batch promote / joint consensus。
- 没有修改 committed voter membership 语义。
- 没有把 learner 计入 commit quorum 或 election quorum。
- 没有修改 proto / 协议语义。
- 没有修改持久化格式。

## Validation

- 构建命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_raft_snapshot_catchup test_raft_snapshot_restart ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：
  - `ctest --preset debug-tests -R "RaftSnapshotCatchup|SnapshotCatchup|RaftSnapshotRestart|SnapshotRestart" --output-on-failure`
- 脚本命令：Not run
- 文件存在性检查：Not run
- 结果：PASS
- 完整日志路径：
  - `tmp/test-logs/t075-snapshot-tests.log`
  - `tmp/test-logs/t075-new-test.log`

## Build Lock

- 使用了 `flock` 构建锁。
- 已获得锁，构建与测试已执行。

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前 runtime progress 仍是诊断用途，未实现 ready-to-promote / waiting-for-pair 状态收口；这部分留给 T076。
- 当前补的是 learner snapshot install 路径，不包含 promote 或 committed membership 变更。

## Result

- 最终状态：PASS
- 可以进入下一任务：是，后续可进入 T076。
