# T069 Add 3 Voters + 1 Learner Quorum Remains 2 Test

## Scope

- 任务类型：测试
- 在 `tests/integrated_object_storage_quorum_test.cpp` 增加 `3 voters + 1 learner` quorum 安全测试。
- 验证 learner 加入后，committed voter quorum 仍按 3 voters 计算，quorum 仍为 2。
- 不实现生产 membership / quorum 逻辑。
- 不实现 learner catch-up / promote-to-voter / batch promote / joint consensus。

## Task Source

- `tasks.md`: T069
- `plan.md`
- `data-model.md`
- `contracts/metadata-learner-join.md`

## Files Changed

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t069-add-3-voters-1-learner-quorum-remains-2-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- 未修改生产代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 新增 `IntegratedObjectStorageQuorumTest.ThreeVotersPlusObservedLearnerKeepsCommittedQuorumAtTwo`。
- 测试通过 `JoinMetadataCluster` leader RPC 接受一个 learner，请求侧明确仍是 `requested_membership=learner_not_voter`，且 `committed_membership_changed=false`。
- 再通过 ViewNode observed metadata 快照构造 `3 voters + 1 learner` 场景，验证：
  - observed 视图中仍然只有 3 个 voters 和 1 个 learner；
  - committed membership 摘要仍保持 `voter_ids=[1,2,3]`、`voter_count=3`、`learner_count=0`、`quorum=2`；
  - 2 个真实 voters 存活时仍可提交；
  - 只剩 1 个真实 voter 时，即使存在 learner，也不能达成 quorum。

## Boundary Checks

- 没有修改生产代码
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 没有实现 learner catch-up
- 没有实现 promote-to-voter
- 没有实现 batch promote / joint consensus
- 没有把 ViewNode 当成 Raft membership authority
- 保持 committed membership authority 仍由 Raft 决定
- 保持 odd voter invariant

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum > tmp/test-logs/t069-build.log 2>&1 ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：`ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure > tmp/test-logs/t069-ctest.log 2>&1`
- 脚本命令：`Not run`
- 文件存在性检查：`test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t069-add-3-voters-1-learner-quorum-remains-2-test.md`
- 结果：`PASS`
- 失败摘要：`None`
- 完整日志路径：
  - `tmp/test-logs/t069-build.log`
  - `tmp/test-logs/t069-ctest.log`

## Build Lock

- 使用 `flock` 构建锁
- 已获得锁
- build/test 未跳过

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前生产代码仍未实现真实 runtime learner membership、learner catch-up 和 promote-to-voter；本测试验证的是现阶段安全边界，不让 learner 被错误计入 committed voter quorum。
- T070 已独立覆盖单 learner promote 被 even voter count 阻止；本任务保持其测试不退化。
- 后续仍需要 T071-T085 等任务补完 runtime learner、quorum、election、catch-up 与 batch promote 实现。

## Result

- 最终状态：`PASS`
- 可以进入下一任务
- 若继续 US4，可进入后续实现任务，或先补充其他剩余测试任务
