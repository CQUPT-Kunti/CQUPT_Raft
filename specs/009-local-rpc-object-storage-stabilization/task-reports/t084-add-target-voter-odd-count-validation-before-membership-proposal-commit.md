# T084 Add Target Voter Odd-Count Validation Before Membership Proposal Commit

## Scope

- 任务类型：实现 / 测试 / 验证
- 本任务在 `RaftNode` 的 committed membership proposal 边界前增加目标 voter odd-count 校验。
- 本任务只处理 target committed voter count 校验、拒绝 even-voter commit proposal，以及对应回归验证。
- 本任务不实现 Metadata service 层的“两 learner 一起 promote 并更新 quorum summary”业务编排；那属于 T085。

## Task Source

- `tasks.md`: T084
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/data-model.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/quickstart.md`

## Files Changed

- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t084-add-target-voter-odd-count-validation-before-membership-proposal-commit.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `tmp/test-logs/t084-validation.log`
- 未修改 proto
- 未修改 CMake
- 未修改持久化格式

## What Changed

- 在 `RaftNode` 私有实现中新增目标 committed voter count 计算与校验 helper：
  - `CommittedVoterCountLocked()`
  - `ValidateTargetCommittedVoterCountLocked()`
  - `ValidateAtomicBatchPromotionTargetsLocked()`
- 在 `PromoteReadyLearnerBatch()` 里，把 odd-count 校验前移到 `PrepareAtomicBatchPromotionLogIndexLocked(...)` 之前：
  - 如果当前 ready learner 只能形成单节点 promote，则先计算 `target committed voter count = current committed voters + 1`
  - 若该目标为偶数，则直接拒绝，不再进入 internal atomic batch promotion log append
- 将 `PrepareAtomicBatchPromotionLogIndexLocked()` 改为接收已验证的 target voter set，并在 append internal command 前再次做 defensive validation。
- 在 `ApplyAtomicBatchPromotionCommand()` 保留 defensive validation，防止任何异常内部命令在 apply 时绕过 odd-count 约束。
- 新增 `IntegratedObjectStorageQuorumTest.SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal`：
  - 直接调用 `RaftNode::PromoteReadyLearnerBatch`
  - 验证 `3 voters + 1 ready learner` 被拒绝
  - 验证拒绝后 `last_log_index`、`commit_index`、committed membership 与 quorum 均保持不变

## Boundary Checks

- 没有修改协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- `raft_node.h` 的修改仅限 private helper 声明；未扩散公共接口
- 没有把 ViewNode 当成 Raft membership authority
- 保持 committed membership authority 仍由 `RaftNode` 内部日志/提交边界决定
- learner 没有被提前计入 voter
- 保持 odd voter invariant
- 保持 no committed 4-voter history 语义

## Validation

- 构建命令：
  - `flock -E 99 -n /tmp/cqupt_raft_build.lock cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum`
  - `flock -E 99 -n /tmp/cqupt_raft_build.lock cmake --build --preset debug-ninja-low-parallel --target test_metadata_failover test_raft_snapshot_restart`
- 测试命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure`
  - `ctest --preset debug-tests -R "RaftSnapshotRestart|SnapshotRestart|MetadataFailover|Failover" --output-on-failure`
- 脚本命令：Not run
- 结果：`PASS`
- 通过摘要：
  - `IntegratedObjectStorageQuorum`: PASS，`14/14`，总耗时 `37.01 sec`
  - `RaftSnapshotRestart|SnapshotRestart|MetadataFailover|Failover`: PASS，`16/16`，总耗时 `104.57 sec`
- 说明：
  - 首次在 sandbox 内执行 `IntegratedObjectStorageQuorum` 时，gRPC 本地端口 bind 被环境权限拦截；改为提权重跑后通过。
  - 第二次 `IntegratedObjectStorageQuorum` 全绿；其中新增 T084 直达回归 `SingleReadyLearnerDirectPromotionIsRejectedBeforeEvenCommittedMembershipProposal` 通过。
- 完整日志路径：
  - `tmp/test-logs/t084-validation.log`

## Build Lock

- 使用了 `flock` 构建锁。
- 获得锁。
- build/test 已实际执行。

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 本任务只保证 target committed voter count 校验前移到 proposal commit 边界；Metadata service 的“双 learner 一起 promote”业务编排仍待 T085。
- 当前批量 promote 仍通过现有 `JoinMetadataCluster`/内部 atomic command 边界路由，后续如果需要 first-class promote API，仍应在 T085/T086 之后单独收口。
- 本任务未发现需要新增同步到 `cross-task-risk-notes.md` 的跨任务风险。

## Result

- 最终状态：`PASS`
- 是否可以进入下一任务：可以
- 下一任务建议：T085
