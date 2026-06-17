# T108 Validate MetadataNode Identity Persistence And Bootstrap Consistency

## 1. 本任务做了什么

- 在 `tests/node_identity_test.cpp` 增加 Metadata bootstrap identity persistence / repeated bootstrap consistency 单测。
- 在 `tests/metadata_client_scenario_test.cpp` 增加真实 `metadata_node_app` 的 clean repeated bootstrap 场景验证。
- 复用现有测试 helper 和 bootstrap 单节点 cluster config 生成路径，验证首次启动、重复启动与 identity 持久化语义保持一致。

## 2. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t108-validate-metadatanode-identity-persistence-and-bootstrap-consistency.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 3. 新增或更新了哪些测试

- 新增：
  - `NodeIdentityTest.T108MetadataBootstrapPersistenceRepeatLoadPrefersPersistedIdentityOverReplacementCreateRequest`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapConsistencyRepeatStartReusesPersistedIdentity`
- 保持不退化的相关已有覆盖：
  - `NodeIdentityTest.T110MetadataBootstrapVoterRestartKeepsIdentityMembershipStateAndPersistentGenerationStable`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRestartRecoveryReusesIdentityAfterForcedExit`

## 4. 如何验证 identity persistence

- unit test 路径：
  - 首次 `LoadOrCreateNodeIdentity()` 创建 bootstrap voter identity。
  - 第二次在同一 `data_dir` 上再次 `LoadOrCreateNodeIdentity()`。
  - 第二次刻意传入不同的 `identity_to_create`，断言实现仍优先复用已持久化的 `node.identity`。
  - 对比前后 identity 文件内容完全一致，确认不会被重复 bootstrap 改写。
- app-level 路径：
  - 真实启动单节点 bootstrap `metadata_node_app`，等待输出 `metadata_node_app OK`。
  - 读取落盘的 `node.identity`，记录 `node_id`、`node_type`、`membership_state`、`persistent_generation`、`source`、`raft_id`。
  - 再次启动同一节点并优雅退出，断言 `node.identity` 内容前后完全一致。

## 5. 如何验证 bootstrap consistency

- 首次 bootstrap 启动断言：
  - identity 文件被正确创建。
  - `node_type=metadata`
  - `membership_state=voter`
  - `persistent_generation=1`
- 重复 bootstrap 断言：
  - 读取同一 `data_dir` 时直接复用既有 identity。
  - 不会因为新的 create 请求或重复启动而重新生成身份。
  - 不会把 replacement identity 或临时配置推断写回 durable identity。

## 6. 如何验证 membership state 保持一致

- unit test 断言 repeated bootstrap 前后：
  - `membership_state` 始终为 `voter`
  - `node_type` 始终为 `metadata`
  - `persistent_generation` 始终为 `1`
- app-level test 断言 repeated bootstrap 前后：
  - `node.identity` 中的 `membership_state` 保持 `voter`
  - ViewNode registry 中对该节点的观测状态保持 `VOTER`
  - `raft_id` 与 endpoint 观测值保持稳定

## 7. 验证结果

- 构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_node_identity test_metadata_client_scenario
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 聚焦回归：
  - `ctest --preset debug-tests -R "T108MetadataBootstrapPersistenceRepeatLoadPrefersPersistedIdentityOverReplacementCreateRequest|MetadataNodeBootstrapConsistencyRepeatStartReusesPersistedIdentity" --output-on-failure`
  - PASS
  - `2/2` tests passed
  - 日志：`tmp/test-logs/t108-focused.log`
- broad 回归：
  - `ctest --preset debug-tests -R "Metadata|NodeIdentity" --output-on-failure`
  - PASS
  - `170/170` tests passed
  - 日志：`tmp/test-logs/t108-metadata-nodeidentity.log`

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T108

- 已勾选

## 10. 是否可以进入下一任务

- 可以。

## 风险和后续注意事项

- 本任务没有修改 MetadataNode 业务逻辑、membership authority、dynamic join、promote 或 quorum 语义。
- app-level bootstrap consistency 用例验证的是 clean repeated bootstrap；异常退出 / restart recovery 语义仍由 T110 已有覆盖负责。
- 本轮未发现新的跨任务风险，因此不需要更新 `cross-task-risk-notes.md`。

## 建议下一步任务

- 建议执行 `T109`。
- 原因：
  - T108 已把 MetadataNode bootstrap identity persistence / consistency 补齐。
  - 下一步应收口 `module-notes.md`，把 009 阶段最终职责边界、状态流转和误用警告同步到文档层。
