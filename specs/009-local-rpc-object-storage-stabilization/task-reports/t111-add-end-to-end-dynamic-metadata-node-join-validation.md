# T111 Add End-To-End Dynamic Metadata Node Join Validation

## 1. 本任务做了什么

- 复核并重新验证仓库中已经存在的 T111 dynamic join 覆盖，没有新增业务功能或协议语义。
- 确认 `tests/node_identity_test.cpp` 和 `tests/metadata_client_scenario_test.cpp` 已覆盖 MetadataNode 从首次启动、join、registry 可见到重启恢复的关键链路。
- 修复了 broad `Metadata|NodeIdentity` 回归中无关的并发时序残余失败：`MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached`。
- 重新执行 T111 相关定向构建、聚焦回归、并发压力重复回归与 broad `Metadata|NodeIdentity` 回归，并按当前结果更新本报告。

## 2. 修改了哪些文件

- `tests/metadata_concurrency_stress_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t111-add-end-to-end-dynamic-metadata-node-join-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

说明：

- T111 所需 direct coverage 仍存在于：
  - `tests/node_identity_test.cpp`
  - `tests/metadata_client_scenario_test.cpp`
- 本轮对 `tests/metadata_concurrency_stress_test.cpp` 做了最小稳定化修正，用于消除 broad 回归中的时序抖动，不涉及业务逻辑、协议语义或持久化格式变更。

## 3. 新增或更新了哪些测试

- 已存在并完成复核的 T111 直接覆盖：
  - `NodeIdentityTest.T111MetadataDynamicJoinJoiningIdentityRestartKeepsMembershipStateAndGenerationStable`
  - `MetadataClientScenarioTest.MetadataNodeDynamicJoinRegistersObservedLearnerAndKeepsIdentityStableAcrossRestart`
- 更新：
  - `MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached`
- 相关动态 join 场景辅助覆盖：
  - `MetadataClientScenarioTest.MetadataNodeCandidateUsesViewLeaderHintBeforeFollowerFallback`
  - `MetadataClientScenarioTest.MetadataNodeCandidateFallsBackToNextDiscoveredMetadataNodeWithoutLeaderHint`
  - `MetadataClientScenarioTest.MetadataNodeCandidateReportsClearFailureWhenAllDiscoveredMetadataCandidatesFail`

## 4. 如何验证 dynamic join 链路

- 通过真实 `metadata_node_app` 候选节点启动路径，验证它先经由 ViewNode `DiscoverMetadata` 获取 leader/candidate 线索，再向 metadata leader 发起 `JoinMetadataCluster`。
- 聚焦用例断言 join 请求会带上稳定 `node_id`、预期 `candidate_raft_id`、非空 `candidate_incarnation_id`、以及稳定的 `persistent_generation`。
- 聚焦用例同时断言 ViewNode registry 最终能够观测到该节点，并且观测到的 membership 角色为 `LEARNER`，不会直接变成 `VOTER`。

## 5. 如何验证 identity 与 membership 状态流转

- `NodeIdentityTest.T111MetadataDynamicJoinJoiningIdentityRestartKeepsMembershipStateAndGenerationStable` 验证：
  - 首次启动时 dynamic join 本地 identity 可以持久化为 `joining`。
  - 重启后继续复用同一个 identity 文件，不会被新的 `identity_to_create` 覆盖。
  - `membership_state` 保持稳定，不会漂移到 `voter`。
- `MetadataClientScenarioTest.MetadataNodeDynamicJoinRegistersObservedLearnerAndKeepsIdentityStableAcrossRestart` 验证：
  - 候选节点对外表达 join/candidate 身份。
  - ViewNode registry 中对该节点的观测状态为 `learner`。
  - 节点不会绕过预期流转直接以 voter 身份出现。

## 6. 如何验证 restart 后状态保持正确

- 首次运行后读取同一 `data_dir` 下的 `node.identity`，记录 `node_id`、`membership_state`、`persistent_generation`。
- 第二次运行同一候选节点进程时，断言：
  - `node_id` 复用，不重新生成。
  - `membership_state` 保持原值，不被重启改写。
  - `persistent_generation` 保持稳定。
  - process incarnation 会更新，证明 durable identity 被复用而不是重新创建。
- 同时验证 ViewNode registry 在重启后仍能继续以 `learner` 观测到该节点，不会因为旧 registry 或恢复顺序导致身份漂移。

## 7. 验证结果

- 定向构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_node_identity test_metadata_client_scenario test_metadata_concurrency_stress
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 聚焦回归：
  - `ctest --preset debug-tests -R "T111MetadataDynamicJoinJoiningIdentityRestartKeepsMembershipStateAndGenerationStable|MetadataNodeDynamicJoinRegistersObservedLearnerAndKeepsIdentityStableAcrossRestart" --output-on-failure`
  - PASS
  - `2/2` tests passed
  - 日志：`tmp/test-logs/t111-focused.log`
- 并发稳定性重复回归：
  - `ctest --preset debug-tests -R "MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached" --repeat until-fail:10 --output-on-failure`
  - PASS
  - `10/10` 通过
  - 日志：`tmp/test-logs/t111-fix-focused-repeat.log`
- broad 回归：
  - `ctest --preset debug-tests -R "Metadata|NodeIdentity" --output-on-failure`
  - PASS
  - `168/168` tests passed
  - 日志：`tmp/test-logs/t111-fix-metadata-nodeidentity.log`

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T111

- 已勾选

## 10. 是否可以进入下一任务

- 可以。
- 原因是 T111 直接相关覆盖、并发稳定性回归和 broad `Metadata|NodeIdentity` 回归已经全部通过。

## 风险和后续注意事项

- 本轮没有改动 metadata 业务逻辑、membership authority、learner/voter 规则或 quorum 语义。
- 当前 T111 直接覆盖已经证明：
  - identity 文件仍是唯一长期身份来源；
  - restart 不会重建 identity；
  - registry 能正确反映 dynamic join 节点的 `learner` 观测状态。
- 对 `MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached` 的修复是测试稳定化修正，不是业务语义修改。
- 本次复核结论支持勾选 T111。

## 建议下一步任务

- 建议下一步执行 `T112`。
- 原因：
  - T111 已完成并验证通过。
  - 现在可以继续推进 Phase 12 文档收口，把已完成任务和验证结果汇总进最终 summary。
