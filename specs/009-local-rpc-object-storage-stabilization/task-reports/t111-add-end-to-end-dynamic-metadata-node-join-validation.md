# T111 Add End-To-End Dynamic Metadata Node Join Validation

## 1. 本任务做了什么

- 在 `tests/node_identity_test.cpp` 中补了 dynamic join `joining` 身份的首次创建与重启复用验证。
- 在 `tests/metadata_client_scenario_test.cpp` 中补了真实 `metadata_node_app` 候选节点启动场景，覆盖：
  - 通过 ViewNode 发现 Metadata leader
  - 发起 join 请求
  - 向 ViewNode 注册/心跳后在 registry 中可见
  - 重启后复用同一持久化 identity，并生成新的 process incarnation
- 同步修正了已有 `metadata_node_app` 场景测试，使其适配当前“后台持续运行 + 周期重试 join”的实现，而不是依赖旧的一次性退出行为。

## 2. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t111-add-end-to-end-dynamic-metadata-node-join-validation.md`

## 3. 新增或更新了哪些测试

- 新增：
  - `NodeIdentityTest.T111MetadataDynamicJoinJoiningIdentityRestartKeepsMembershipStateAndGenerationStable`
  - `MetadataClientScenarioTest.MetadataNodeDynamicJoinRegistersObservedLearnerAndKeepsIdentityStableAcrossRestart`
- 更新：
  - `MetadataClientScenarioTest.MetadataNodeCandidateUsesViewLeaderHintBeforeFollowerFallback`
  - `MetadataClientScenarioTest.MetadataNodeCandidateFallsBackToNextDiscoveredMetadataNodeWithoutLeaderHint`
  - `MetadataClientScenarioTest.MetadataNodeCandidateReportsClearFailureWhenAllDiscoveredMetadataCandidatesFail`

## 4. 如何验证 dynamic join 链路

- 先用内存 ViewNode gRPC server 预置一个已知 leader 的 metadata 候选节点。
- 启动真实 `metadata_node_app --node_id meta-candidate-1`，让它通过 `DiscoverMetadata` 找到 leader，再向 fake `JoinMetadataCluster` leader 发起 join。
- 断言 join 请求中带有：
  - 稳定 `node_id`
  - 预期 `candidate_raft_id`
  - `persistent_generation=1`
  - `local_state_hint=CANDIDATE`
  - 非空 `candidate_incarnation_id`
- 断言 ViewNode registry 中随后出现 `meta-candidate-1`，且观测到的状态是 `LEARNER`，不是 `VOTER`。

## 5. 如何验证 identity 与 membership 状态流转

- `node_identity_test` 验证：
  - dynamic join 本地 identity 允许 `joining`
  - 重启不会把 `joining` 覆盖成其他状态
  - `persistent_generation` 保持不变
  - 不会被新的 `identity_to_create` 静默替换
- `metadata_client_scenario_test` 验证：
  - 本地持久化 identity 仍保持 `membership_state=candidate`
  - join 请求对外表达 `candidate` 状态
  - ViewNode registry 中对该节点的观测状态变成 `learner`
  - 节点不会直接以 `voter` 身份出现

## 6. 如何验证 restart 后状态保持正确

- 首次运行后读取 `node.identity`，记录：
  - `node_id`
  - `membership_state`
  - `persistent_generation`
  - `source`
- 第二次运行同一 `metadata_node_app`：
  - 断言 `node.identity` 内容保持不变
  - 断言新的 join 请求仍使用同一 `node_id`、同一 `persistent_generation`
  - 断言新的 `candidate_incarnation_id` 与首次运行不同，说明 restart 复用 durable identity 但生成了新的 process incarnation
  - 断言 ViewNode registry 中该节点仍以 `learner` 观测状态存在，不会漂移成 `voter`

## 7. 验证结果

- 构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_node_identity test_metadata_client_scenario
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 重点场景回归：
  - `ctest --preset debug-tests -R "MetadataNodeCandidateUsesViewLeaderHintBeforeFollowerFallback|MetadataNodeCandidateFallsBackToNextDiscoveredMetadataNodeWithoutLeaderHint|MetadataNodeDynamicJoinRegistersObservedLearnerAndKeepsIdentityStableAcrossRestart" --output-on-failure`
  - PASS
  - 日志：`tmp/test-logs/t111-metadata-app-scenarios.log`
- broad 回归：
  - `ctest --preset debug-tests -R "Metadata|NodeIdentity" --output-on-failure`
  - FAIL
  - 第一次日志：`tmp/test-logs/t111-metadata-nodeidentity-ctest.log`
  - 第二次日志：`tmp/test-logs/t111-metadata-nodeidentity-ctest-rerun.log`
- 相关复查：
  - `ctest --preset debug-tests -R "ConcurrentDuplicateCreateObjectRequestsShareSameLogIndex|AdmissionRejectsWhenInflightLimitIsReached" --output-on-failure`
  - PASS
  - 日志：`tmp/test-logs/t111-rerun-flaky-metadata.log`
  - `ctest --preset debug-tests -R "RestartRecoveryAfterConcurrentWritesKeepsCommittedAndDeletedMetadataStable" --output-on-failure`
  - FAIL
  - 日志：`tmp/test-logs/t111-rerun-recovery-stress.log`

## 8. 是否 PASS / FAIL / SKIPPED

- FAIL

## 9. 是否已勾选 T111

- 未勾选

## 10. 是否可以进入下一任务

- 不建议直接进入下一任务。
- 新增的 dynamic join 覆盖本身已经通过，但用户要求的 broad `Metadata|NodeIdentity` 验证未获得干净 PASS，当前仍有既有 metadata failover / concurrency / recovery 测试不稳定问题需要先收口。

## 风险和后续注意事项

- 本次改动没有修改 metadata 业务逻辑、membership 决策逻辑或 promote/quorum 行为，新增覆盖集中在测试与最小测试 helper。
- `metadata_node_app` 当前 candidate 模式会在后台持续重试 join；因此相关 app 场景测试不能再假设“join 成功后立即退出”，必须显式终止进程并基于输出/请求/registry 状态断言。
- 当前 broad `Metadata|NodeIdentity` 套件里仍存在与本任务修改范围无关的 timing-sensitive 失败，已同步记录到 `cross-task-risk-notes.md`。
