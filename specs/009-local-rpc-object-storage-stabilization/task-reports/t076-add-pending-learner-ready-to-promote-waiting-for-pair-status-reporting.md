# T076 Task Report

## 任务

- ID: `T076`
- 标题: `Add pending learner / ready-to-promote / waiting-for-pair status reporting in modules/raft/service/metadata_service_impl.cpp`
- 结果: `PASS`

## 本次修改

- 在 `modules/raft/service/metadata_service_impl.cpp` 的 `JoinMetadataCluster` 响应摘要中，追加基于 `RaftNode::GetRuntimeMembershipSummary()` 的 learner 诊断输出。
- 诊断输出仅做状态汇报，不修改 committed membership、不修改 quorum、不触发 promote。
- 在 `tests/integrated_object_storage_quorum_test.cpp` 新增真实 pending learner catch-up 后的状态可观察测试。

## 状态汇报语义

- `learner_status=pending`
  - AddLearner 刚被 leader 接受、runtime learner 已存在，但尚未追到 committed log 边界。
- `learner_status=ready_to_promote`
  - runtime learner 的已观测进度达到当前 committed log 边界。
- `promotion_status=waiting_for_pair`
  - 单个 ready learner 若直接 promote 会把 `3 voters` 变成 `4 voters`，因此只汇报为等待配对，不提交 membership change。
- 额外诊断字段：
  - `runtime_voter_count`
  - `runtime_learner_count`
  - `learner_match_index`
  - `learner_next_index`
  - `learner_ready_index`
  - `promotion_block_reason=even_voter_count`
  - `promotion_policy=odd_committed_voter_count_only`

## 为什么不改变 committed voter set / quorum

- 诊断仍然同时附带 `committed_voter_count`、`committed_quorum_size` 和 `quorum_rule=committed_membership_majority_only`。
- learner 状态来自 runtime summary，只读观察，不写入 committed membership。
- 单个 ready learner 只会得到 `waiting_for_pair` 诊断，不会把 committed voter count 变成 `4`。

## 新增测试

- `IntegratedObjectStorageQuorumTest.JoinMetadataClusterReportsPendingThenReadyLearnerWaitingForPair`

## 测试验证点

- 第一次 `JoinMetadataCluster`：
  - 返回 `ACCEPTED_PENDING_COMMIT`
  - `summary.message` 包含 `learner_status=pending`
  - `summary.message` 包含 `promotion_status=catching_up`
- 启动 detached learner 并完成日志追赶后，重放同一个 join 请求：
  - 返回 `DUPLICATE`
  - `summary.message` 包含 `learner_status=ready_to_promote`
  - `summary.message` 包含 `promotion_status=waiting_for_pair`
  - `summary.message` 包含 `promotion_block_reason=even_voter_count`
- committed voter set 仍保持 `3 voters`
- committed quorum 仍保持 `2`

## 未实现内容确认

- 未实现 `promote-to-voter`
- 未实现 `batch promote`
- 未实现 `joint consensus`
- 未修改 committed membership change 语义
- 未修改 committed-voters-only quorum 规则

## 修改文件

- `modules/raft/service/metadata_service_impl.cpp`
- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t076-add-pending-learner-ready-to-promote-waiting-for-pair-status-reporting.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 验证

### 构建

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_quorum test_metadata_client_scenario ) 9>/tmp/cqupt_raft_build.lock
```

- 结果: `PASS`

### 测试

```bash
ctest --preset debug-tests -R "IntegratedObjectStorageQuorum|MetadataClientScenario" --output-on-failure
```

- 结果: `PASS`

### 备注

- 按用户建议命令中的 `metadata_client_scenario` 在当前仓库中对应真实 build target `test_metadata_client_scenario`，因此验证时使用真实 target 名称。
- 首轮聚合 CTest 出现一次现有 `T070` 用例时序抖动；单独复跑通过后，整组 `IntegratedObjectStorageQuorum|MetadataClientScenario` 再跑一轮全量通过，最终按 `PASS` 记录。
