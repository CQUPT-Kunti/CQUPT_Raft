# T070 Add Single Learner Promote Blocked By Even Voter Count Test

## Scope

- 任务类型：测试
- 在 `tests/integrated_object_storage_quorum_test.cpp` 增加单 learner promote 安全边界测试。
- 验证 `3 voters + 1 learner` 不会直接变成 committed `4 voters`，learner 仍不参与 voter quorum。
- 不实现 promote-to-voter。
- 不实现 batch promote / joint consensus。
- 不修改生产 membership / quorum 逻辑。

## Task Source

- `tasks.md`: T070
- `plan.md`
- `data-model.md`
- `contracts/metadata-learner-join.md`

## Files Changed

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t070-add-single-learner-promote-blocked-by-even-voter-count-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- 未修改生产代码
- 未修改 proto
- 未修改 CMake

## What Changed

- 扩展测试 helper，使本测试文件可以显式构造 observed learner 角色。
- 新增 `IntegratedObjectStorageQuorumTest.SingleObservedLearnerDoesNotAutoPromoteToEvenCommittedVoterCount`。
- 测试先通过 `AddLearner` 提案路径接受一个 learner，再向 ViewNode 注册一个 observed learner，最后验证 committed membership 仍固定为 3 voters、quorum 仍为 2。
- 测试同时验证：
  - 2 个真实 voters 存活时仍可提交；
  - 只剩 1 个真实 voter 时，即使存在 pending / observed learner，也不能达成 quorum；
  - observed learner 不会把 committed voter count 静默推成 4。

## Boundary Checks

- 没有修改生产代码
- 没有修改 proto / 协议语义
- 没有修改持久化格式
- 没有修改公共 API 行为
- 没有实现 promote-to-voter
- 没有实现 batch promote / joint consensus
- 没有把 ViewNode 当成 Raft membership authority
- 保持 committed membership authority 仍由 Raft 决定
- 保持 odd voter invariant

## Validation

- 构建命令：`( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum > tmp/test-logs/t070-build.log 2>&1 ) 9>/tmp/cqupt_raft_build.lock`
- 测试命令：`ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure > tmp/test-logs/t070-ctest.log 2>&1`
- 脚本命令：`Not run`
- 文件存在性检查：`test -f specs/009-local-rpc-object-storage-stabilization/task-reports/t070-add-single-learner-promote-blocked-by-even-voter-count-test.md`
- 结果：`PASS`
- 失败摘要：`None`
- 完整日志路径：
  - `tmp/test-logs/t070-build.log`
  - `tmp/test-logs/t070-ctest.log`

## Build Lock

- 使用 `flock` 构建锁
- 已获得锁
- build/test 未跳过

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前生产代码仍未实现真实 promote-to-voter，因此本测试表达的是安全边界：单 learner 不能静默进入 committed 4-voter 配置。
- 后续需要由 T071-T085 系列任务实现 runtime learner、quorum、catch-up、batch promote / joint consensus。
- T069 仍未完成；本任务没有替代 `3 voters + 1 learner quorum remains 2` 的独立测试职责。

## Result

- 最终状态：`PASS`
- 可以进入下一任务
- 后续若继续 US4，优先补 T069 或后续 promote/batch 相关任务
