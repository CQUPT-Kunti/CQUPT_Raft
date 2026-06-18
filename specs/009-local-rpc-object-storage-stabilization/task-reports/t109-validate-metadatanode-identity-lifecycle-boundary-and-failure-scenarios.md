# T109 Validate MetadataNode Identity Lifecycle Boundary And Failure Scenarios

## 1. 本任务做了什么

- 在 `tests/node_identity_test.cpp` 补充 MetadataNode bootstrap identity 缺失与非法 `persistent_generation` 边界验证。
- 在 `tests/metadata_client_scenario_test.cpp` 补充真实 `metadata_node_app` 针对非法 `node.identity` 的启动拒绝场景验证。
- 覆盖 identity 损坏、node type 不匹配、membership state 非法、persistent generation 非法等 MetadataNode identity 生命周期边界。

## 2. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t109-validate-metadatanode-identity-lifecycle-boundary-and-failure-scenarios.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`

## 3. 新增或更新了哪些测试

- 新增 unit tests：
  - `NodeIdentityTest.T109MetadataBootstrapMissingIdentityCreatesFreshDurableIdentity`
  - `NodeIdentityTest.T109MetadataBootstrapRejectsPersistedZeroGenerationWithoutOverwrite`
- 新增 app-level tests：
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRejectsCorruptPersistedIdentityWithoutSilentRewrite`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRejectsPersistedStorageIdentityRoleMismatch`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRejectsPersistedInvalidMembershipState`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRejectsPersistedInvalidPersistentGeneration`
- 保持不退化的相关已有覆盖：
  - `NodeIdentityTest.T108MetadataBootstrapPersistenceRepeatLoadPrefersPersistedIdentityOverReplacementCreateRequest`
  - `NodeIdentityTest.T110MetadataBootstrapVoterRestartKeepsIdentityMembershipStateAndPersistentGenerationStable`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapConsistencyRepeatStartReusesPersistedIdentity`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRestartRecoveryReusesIdentityAfterForcedExit`

## 4. 如何验证 identity 边界场景

- 缺失 identity：
  - 在空 `data_dir` 上执行 Metadata bootstrap `LoadOrCreateNodeIdentity()`。
  - 断言首次启动会创建 durable identity，而不是依赖隐式推断漂移身份。
- bootstrap 不覆盖已有身份：
  - 依赖现有 T108/T110 覆盖，验证重复启动和 restart 时都复用已有 `node.identity`。
- 本地文件不能伪造合法 voter 身份：
  - 对 dynamic join/local override 非法 voter 持久化约束继续由现有 T055/T009 覆盖。

## 5. 如何验证 identity 异常场景

- identity 内容损坏：
  - 预写入明显损坏的 `node.identity`，启动真实 `metadata_node_app`。
  - 断言进程以 identity error 退出，不继续启动。
- node type 不匹配：
  - 预写入 `node_type=storage` 的持久化 identity，再按 MetadataNode 启动。
  - 断言启动阶段明确拒绝复用该 identity。
- membership state 非法：
  - 预写入非法 `membership_state`，断言启动失败并保留原文件。
- persistent generation 非法：
  - 预写入 `persistent_generation=0`。
  - unit test 断言 `LoadOrCreateNodeIdentity()` 失败且不重写文件。
  - app-level test 断言 `metadata_node_app` 直接拒绝启动。

## 6. 如何验证非法 identity 被拒绝

- 对所有非法 identity 场景统一断言：
  - `metadata_node_app` 返回 identity error 退出码 `4`
  - 输出包含 `node.identity startup check failed`
  - 原始 `node.identity` 文件内容保持不变
- 对 unit test 断言：
  - 返回非 `ok()`
  - 不会 `loaded_existing`
  - 不会 `created_new`
  - 包含对应 issue code，例如：
    - `kInvalidPersistentGeneration`
    - `kNodeTypeMismatch`
    - `kInvalidMembershipState`
    - `kIdentityFileCorrupt`

## 7. 验证结果

- 构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_node_identity test_metadata_client_scenario
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 聚焦回归：
  - `ctest --preset debug-tests -R "T109MetadataBootstrapMissingIdentityCreatesFreshDurableIdentity|T109MetadataBootstrapRejectsPersistedZeroGenerationWithoutOverwrite|MetadataNodeBootstrapRejectsCorruptPersistedIdentityWithoutSilentRewrite|MetadataNodeBootstrapRejectsPersistedStorageIdentityRoleMismatch|MetadataNodeBootstrapRejectsPersistedInvalidMembershipState|MetadataNodeBootstrapRejectsPersistedInvalidPersistentGeneration" --output-on-failure`
  - PASS
  - `6/6` tests passed
  - 日志：`tmp/test-logs/t109-focused.log`
- broad 回归：
  - `ctest --preset debug-tests -R "Metadata|NodeIdentity" --output-on-failure`
  - PASS
  - `176/176` tests passed
  - 日志：`tmp/test-logs/t109-metadata-nodeidentity-rerun.log`

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T109

- 已勾选

## 10. 是否可以进入下一任务

- 可以。

## 风险和后续注意事项

- 本任务没有修改 MetadataNode 业务逻辑、dynamic join、promote、quorum 或 membership authority 语义。
- 当前 T109 已经证明：
  - 非法 identity 会被发现并拒绝；
  - `metadata_node_app` 不会静默修复或覆盖坏 identity；
  - restart / bootstrap 不会通过本地文件导致身份漂移。
- broad 回归首轮观测到一次 `MetadataRecoveryStressTest.RestartRecoveryAfterConcurrentWritesKeepsCommittedAndDeletedMetadataStable` 的时序性失败，单测复跑和 broad 复跑均已通过；该 residual risk 已同步记录到 `cross-task-risk-notes.md`。

## 建议下一步任务

- 建议执行 `T112` 之前，先按当前 `tasks.md` 语义恢复并完成 `module-notes.md` 的最终收口任务。
- 如果继续沿用你这轮口头任务编号顺序，则建议下一步执行 `T110` 之后的文档收口项；如果按仓库当前 `tasks.md` 顺序推进，建议下一步处理 `T112` 或补回独立的 module-notes 收口任务。
