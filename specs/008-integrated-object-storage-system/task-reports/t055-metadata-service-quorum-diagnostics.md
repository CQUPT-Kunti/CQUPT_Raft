# T055 MetadataService Quorum / Leader Diagnostics 报告

## 1. 修改了哪些文件

- `modules/raft/service/metadata_service_impl.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t055-metadata-service-quorum-diagnostics.md`

未修改：

- `modules/raft/service/metadata_service_impl.h`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `proto/*`
- 测试文件

## 2. MetadataService quorum / leader diagnostics 映射做了什么

本任务只在 `MetadataService` 响应层补充诊断映射，没有改动 Raft 共识语义。

具体实现：

- 在写请求响应中，对 `NOT_LEADER`、`TIMEOUT`、`NODE_STOPPING`、`COMMIT_FAILED` 以及“failed to replicate log entry to majority”这类多数派复制失败场景，附加只读 quorum / leader 诊断信息。
- 诊断信息来自 `RaftNode::GetCommittedMembershipQuorumSummary()`，用于把以下内容拼入 `summary.message`：
  - `leader_hint_id`
  - `leader_hint_address`
  - `committed_voter_count`
  - `committed_quorum_size`
  - `local_committed_membership_role`
  - `committed_membership_index`
  - `committed_membership_term`
  - `committed_voter_ids`
  - `quorum_rule=committed_membership_majority_only`
- 在读请求 admission 拒绝路径中，对 `NOT_LEADER`、`TIMEOUT`、`SERVICE_UNAVAILABLE` 同样补充上述诊断。
- 对 leader hint 做了兜底处理：优先使用当前节点快照中的 leader 信息，必要时回退到 `ProposeResult.leader_id`。
- 对多数派复制失败和 commit 失败的响应码做了更清晰的响应层映射：
  - 多数派复制失败前缀 `"failed to replicate log entry to majority"` 映射为 `METADATA_STATUS_CODE_SERVICE_UNAVAILABLE`
  - `kCommitFailed` 映射为 `METADATA_STATUS_CODE_SERVICE_UNAVAILABLE`

## 3. 是否确认不改变 election / commit / membership 行为

已确认不改变。

- 没有修改 Raft election 逻辑
- 没有修改 Raft commit 逻辑
- 没有修改 membership / quorum 计算逻辑
- 没有把 ViewNode 观测状态作为 membership authority
- 只读取 T053/T054 暴露的 quorum summary 做响应诊断，不参与任何提交判定

## 4. 是否有 disabled/scaffold 测试；是否发现不合理点 / 警告 / 风险

没有新增 disabled/scaffold 测试。

发现的风险/注意点：

- 当前对“多数派复制失败”的更细诊断映射依赖 `ProposeResult.message` 的固定前缀 `"failed to replicate log entry to majority"`；如果后续内部报错文案调整，诊断分类可能退化，但不会影响 Raft 正确性。
- 这次任务没有扩展 proto 字段，而是复用现有 `summary.message` 和 `leader_hint` 承载诊断；兼容性较好，但结构化程度有限。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 6. 验证命令和结果

执行的验证命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core'
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --build --preset debug-ninja-safe --target test_metadata_failover test_metadata_client_scenario'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "^(MetadataFailoverTest\\.(FollowerWriteReturnsNotLeader|FollowerHeadAndListReturnNotLeader)|MetadataClientScenarioTest\\.(ClientShowsRetryableAdmissionStatuses|ReadCommandsShowRetryableAdmissionStatuses))$" --output-on-failure'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "^MetadataFailoverTest\\.LeaderWriteTimeoutReturnsTimeoutAndSameRequestIdCanRetry$" --output-on-failure'
git diff -- modules/raft/service/metadata_service_impl.cpp modules/raft/service/metadata_service_impl.h modules/raft/node/raft_node.h modules/raft/node/raft_node.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t055-metadata-service-quorum-diagnostics.md
```

结果：

- `raft_core` 构建通过
- `test_metadata_failover` / `test_metadata_client_scenario` 构建通过
- 以下 5 个定向测试 PASS：
  - `MetadataFailoverTest.FollowerWriteReturnsNotLeader`
  - `MetadataFailoverTest.FollowerHeadAndListReturnNotLeader`
  - `MetadataFailoverTest.LeaderWriteTimeoutReturnsTimeoutAndSameRequestIdCanRetry`
  - `MetadataClientScenarioTest.ClientShowsRetryableAdmissionStatuses`
  - `MetadataClientScenarioTest.ReadCommandsShowRetryableAdmissionStatuses`

本地日志文件：

- `tmp/test-logs/t055-build.log`
- `tmp/test-logs/t055-test-build.log`
- `tmp/test-logs/t055-ctest.log`
- `tmp/test-logs/t055-timeout-ctest.log`
