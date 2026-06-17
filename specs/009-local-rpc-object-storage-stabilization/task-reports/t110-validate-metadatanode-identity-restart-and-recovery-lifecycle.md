# T110 Validate MetadataNode Identity Restart And Recovery Lifecycle

## 1. 本任务做了什么

- 在 `tests/node_identity_test.cpp` 补充 Metadata bootstrap voter identity 的 restart/recovery 生命周期验证。
- 在 `tests/metadata_client_scenario_test.cpp` 补充真实 `metadata_node_app` 的启动、被强制退出、重启恢复场景验证。
- 复用现有测试 helper，最小扩展了 `metadata_node_app` 的定时终止能力和单节点 bootstrap cluster config 生成能力。

## 2. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t110-validate-metadatanode-identity-restart-and-recovery-lifecycle.md`

## 3. 新增或更新了哪些测试

- 新增：
  - `NodeIdentityTest.T110MetadataBootstrapVoterRestartKeepsIdentityMembershipStateAndPersistentGenerationStable`
  - `MetadataClientScenarioTest.MetadataNodeBootstrapRestartRecoveryReusesIdentityAfterForcedExit`
- 更新：
  - `tests/metadata_client_scenario_test.cpp` 中的进程 helper，支持按指定信号结束 `metadata_node_app`，用于模拟异常退出后的恢复验证。

## 4. 如何验证 identity restart/recovery 生命周期

- unit test 路径：
  - 首次创建 bootstrap voter identity。
  - 第二次 `LoadOrCreateNodeIdentity()` 读取同一 `data_dir`。
  - 断言不会使用新的 `identity_to_create` 覆盖原 identity。
- app-level 路径：
  - 真实启动单节点 bootstrap `metadata_node_app`，等待输出 `metadata_node_app OK`。
  - 在 identity 已落盘、RPC 已启动后，由测试发送 `SIGKILL` 强制退出，模拟异常退出。
  - 再次启动同一 app，验证它直接复用原 `node.identity` 并恢复为同一 MetadataNode 身份。

## 5. 如何验证 membership state 保持稳定

- unit test 断言：
  - 首次 identity 为 `membership_state=voter`
  - 重启后仍为 `membership_state=voter`
- app-level test 断言：
  - `node.identity` 文件中的 `membership_state` 首次启动后为 `voter`
  - 强制退出并重启后仍为 `voter`
  - ViewNode registry 中重启前后对该节点的观测 `membership_state` 都保持 `VOTER`

## 6. 如何验证 persistent generation 保持正确

- 读取 `node.identity` 文件中的 `persistent_generation` 字段。
- 断言首次启动为 `1`。
- 断言重启恢复后仍为 `1`，且整个 `node.identity` 文件内容与首次启动后完全一致，没有重新生成或静默改写。

## 7. 验证结果

- 构建：
  - `(
       flock -n 9 || exit 99
       cmake --build --preset debug-ninja-low-parallel --target test_node_identity test_metadata_client_scenario
     ) 9>/tmp/cqupt_raft_build.lock`
  - PASS
- 聚焦回归：
  - `ctest --preset debug-tests -R "T110MetadataBootstrapVoterRestartKeepsIdentityMembershipStateAndPersistentGenerationStable|MetadataNodeBootstrapRestartRecoveryReusesIdentityAfterForcedExit" --output-on-failure`
  - PASS
  - 日志：`tmp/test-logs/t110-focused.log`
- broad 回归：
  - `ctest --preset debug-tests -R "Metadata|NodeIdentity" --output-on-failure`
  - PASS
  - `168/168` tests passed
  - 日志：`tmp/test-logs/t110-metadata-nodeidentity.log`

## 8. 是否 PASS / FAIL / SKIPPED

- PASS

## 9. 是否已勾选 T110

- 已勾选

## 10. 是否可以进入下一任务

- 可以

## 风险和后续注意事项

- 本任务没有修改 MetadataNode 业务逻辑、membership authority 或动态 join 语义，只补了 restart/recovery 生命周期验证。
- 当前 app-level recovery 验证用 `SIGKILL` 模拟异常退出，已足够证明 identity 文件是唯一长期身份来源；后续如需要更强 crash window 覆盖，可再补更细粒度的 failure injection。
- 本轮未发现新的跨任务风险，不需要额外更新 `cross-task-risk-notes.md`。

## 建议下一步任务

- 建议执行 `T111`。
- 原因：
  - `T110` 已经把 MetadataNode 的 bootstrap identity restart/recovery 生命周期收口。
  - 下一步自然衔接到 dynamic join 端到端验证，继续覆盖 candidate/joining 状态、registry 可见性和重启后身份稳定性。
