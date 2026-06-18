# T085 Promote Two Ready Learners Together And Update Quorum Summary

## Scope

- 任务类型：实现 / 测试 / 验证
- 本任务在 `MetadataServiceImpl` 接通两个 ready learners 的成组 promote 路由，并让成功响应显式反映 batch promote 与更新后的 quorum summary。
- 本任务不实现新的 joint consensus 协议，不修改 `RaftNode` 的 membership authority，不扩展 local RPC example。

## Task Source

- `tasks.md`: T085
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/data-model.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t084-add-target-voter-odd-count-validation-before-membership-proposal-commit.md`

## Files Changed

- `modules/raft/service/metadata_service_impl.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t085-promote-two-ready-learners-together-and-update-quorum-summary.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `tmp/test-logs/t085-validation.log`
- 未修改 `modules/raft/service/metadata_service_impl.h`
- 未修改 proto
- 未修改 CMake

## What Changed

- 收紧了 service 层 duplicate join request 到 batch promote 的路由条件：
  - 只有当前请求对应 learner 已 ready，且 runtime ready learner 数量正好为 `2` 时，`JoinMetadataCluster` 才会通过 `RaftNode::PromoteReadyLearnerBatch(...)` 触发成组 promote。
  - 这样 service 层不会在未来出现 `>2` ready learners 时任意挑选 promote pair；该并发/重复场景留给 T086 收口。
- 保留单 learner ready 时的 `waiting_for_pair` / `even_voter_count` 诊断，不绕过 T084。
- 在 promote 成功响应上追加 service 侧状态装饰：
  - `learner_status=promoted`
  - `promotion_status=batch_promoted`
  - `promotion_batch_size=2`
  - 同时仍保留 `committed_voter_count=5`、`committed_quorum_size=3`、`runtime_voter_count=5`、`runtime_learner_count=0`
- 更新了 `integrated_object_storage_quorum_test.cpp`，把 batch promote 成功响应的新 service 诊断字段纳入断言。

## Boundary Checks

- 没有修改 `RaftNode` business logic
- 没有绕过 T084 odd-count validation
- 没有把 ViewNode observation 当成 membership authority
- 没有修改持久化格式
- 没有修改公共 API 结构
- 没有引入 committed 4-voter 中间态

## Validation

- 构建命令：
  - `flock -E 99 -n /tmp/cqupt_raft_build.lock cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum`
  - `flock -E 99 -n /tmp/cqupt_raft_build.lock cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario`
- 测试命令：
  - `ctest --preset debug-tests -R "IntegratedObjectStorageQuorum" --output-on-failure`
  - `ctest --preset debug-tests -R "MetadataClientScenario" --output-on-failure`
- 脚本命令：Not run
- 结果：`PASS`
- 通过摘要：
  - `IntegratedObjectStorageQuorum`: PASS，`14/14`，总耗时 `47.56 sec`
  - `MetadataClientScenario`: PASS，`14/14`，总耗时 `0.79 sec`
- 完整日志路径：
  - `tmp/test-logs/t085-validation.log`

## Build Lock

- 使用了 `flock` 构建锁
- 已获得锁
- 构建和测试均已执行

## Platform Notes

- Linux：已验证
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前 service 层只把“正好两个 ready learners”的 promote pair 视为 T085 的安全路径；更复杂的并发/重复 pending membership 组合仍待 T086。
- `JoinMetadataCluster` 仍是当前最小 promote 路由边界；如果后续需要 operator-facing 的 first-class promote RPC，应在后续任务中单独设计，不继续过载 join 语义。
- 本任务未发现需要新增到 `cross-task-risk-notes.md` 的新跨任务风险。

## Result

- 最终状态：`PASS`
- 是否可以进入下一任务：可以
- 下一任务建议：T086
