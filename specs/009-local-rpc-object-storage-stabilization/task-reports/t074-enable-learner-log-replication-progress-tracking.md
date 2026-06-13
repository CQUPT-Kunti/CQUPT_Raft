# T074 Enable Learner Log Replication Progress Tracking

## Scope

- 任务类型：实现
- 在 `modules/raft/replication/replicator.h` 和 `modules/raft/replication/replicator.cpp` 接入 learner log replication progress tracking。
- 允许最小同步 `modules/raft/node/raft_node.h`、`modules/raft/node/raft_node.cpp` 与 `tests/test_raft_log_replication.cpp`。
- 不实现 learner InstallSnapshot catch-up。
- 不实现 promote-to-voter。
- 不实现 batch promote / joint consensus。

## Task Source

- `tasks.md`: T074
- `plan.md`
- `data-model.md`
- `contracts/metadata-learner-join.md`

## Files Changed

- `modules/raft/replication/replicator.h`
- `modules/raft/replication/replicator.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/test_raft_log_replication.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t074-enable-learner-log-replication-progress-tracking.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- 未修改 proto
- 未修改 example 脚本
- 未修改 integrated quorum 测试文件

## What Changed

- 为 `Replicator` 增加 replication target role，显式区分 committed voter target 与 learner target。
- learner target 的 `AppendEntries` 成功后，会更新对应 `match_index` / `next_index` 进度，但不会推进 `commit_index`，也不会触发 `should_apply`。
- learner target 需要 snapshot 才能继续追赶时，当前阶段直接停在 AppendEntries 边界，不自动进入 snapshot install 路径，留给 T075。
- `RaftNode` 为 pending learner 建立临时 client / replicator / `match_index` / `next_index` 状态，并在 heartbeat 路径里对 learner 做 best-effort log replication。
- runtime membership 诊断现在会把 learner 的 `match_index` / `next_index` 带出来，便于测试确认 progress 推进。
- pending learner 被清理或 leader 角色切换时，会同步回收对应 replication state，避免 stale progress 污染新状态。

## Boundary Checks

- 没有实现 learner InstallSnapshot catch-up
- 没有实现 promote-to-voter
- 没有实现 batch promote / joint consensus
- 没有修改 committed voter membership 语义
- 没有把 learner 计入 commit majority
- 没有把 learner 计入 election majority
- 没有让 ViewNode 成为 membership authority
- 保持 T072 committed-voters-only quorum 语义

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_raft_log_replication integrated_object_storage_quorum > tmp/test-logs/t074-build.log 2>&1 ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：`ctest --preset debug-tests -R "RaftLogReplication|LogReplication|IntegratedObjectStorageQuorum" --output-on-failure > tmp/test-logs/t074-ctest.log 2>&1`
- 脚本命令：`Not run`
- 文件存在性检查：`test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t074-enable-learner-log-replication-progress-tracking.md`
- 结果：`PASS`
- 失败摘要：`None`
- 完整日志路径：
  - `tmp/test-logs/t074-build.log`
  - `tmp/test-logs/t074-ctest.log`

## Build Lock

- 使用 `flock` 构建锁
- 已获得锁
- build/test 未跳过

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- learner progress 已经能通过 AppendEntries 推进，但 snapshot install catch-up 仍未实现；如果 learner 落后到需要 snapshot，当前阶段不会自动完成追赶，这部分留给 T075。
- 当前实现只记录和推进 learner 复制进度，不改变 quorum / election / promote 语义；后续 promote 安全仍需后续任务完成。

## Result

- 最终状态：`PASS`
- 可以进入下一任务
- 下一步可进入 T075
