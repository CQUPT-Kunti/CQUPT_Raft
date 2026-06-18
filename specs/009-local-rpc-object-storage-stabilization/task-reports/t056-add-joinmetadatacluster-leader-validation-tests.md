## T056

### 做了什么

- 在 `tests/metadata_client_scenario_test.cpp` 增加 JoinMetadataCluster leader validation 的测试先行收口。
- 不实现 JoinMetadataCluster 生产逻辑，只锁定当前 authority 边界和后续 T059/T060 必须满足的 leader validation 约束。

### 修改文件

- `tests/metadata_client_scenario_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t056-add-joinmetadatacluster-leader-validation-tests.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

### 新增或更新了哪些 JoinMetadataCluster leader validation 测试

- `MetadataClientScenarioTest.JoinMetadataClusterContractIsNotYetExposedByMetadataServiceProto`
  - 断言当前 `MetadataService` proto 还没有 `JoinMetadataCluster` / `AddLearner` method。
  - 这把 T056 当前阶段的缺口显式固定下来，避免后续把普通 metadata RPC 或 ViewNode observation 当成 join authority。

- `MetadataClientScenarioTest.UnsupportedJoinMetadataClusterCliDoesNotBypassLeaderAuthority`
  - 断言当前 `raft_metadata_client` 不接受 `join-metadata-cluster` 命令。
  - 这保证在正式 join RPC 未实现前，不存在绕过 Metadata leader validation 的隐式 CLI 入口。

- `MetadataClientScenarioTest.FutureJoinLeaderValidationMustReturnNotLeaderAndLeaderHintForFollowerAuthority`
  - 复用当前 metadata scenario fake server 的 authority rejection 语义，断言非 leader / non-authority 路径返回 `NOT_LEADER`、`retryable=true` 和 `leader_address`。
  - 这为后续 JoinMetadataCluster RPC 明确了 leader validation 的最小行为边界。

### 如何证明非 leader 不能接受 join authority

- 当前 proto 不提供 `JoinMetadataCluster` / `AddLearner` method，因此测试先锁定“能力缺失不能被冒充为已存在 authority”。
- 当前 CLI 不支持 `join-metadata-cluster`，所以 candidate identity/config 不能经由普通 metadata client 命令偷偷绕过 leader validation。
- 对 authority rejection 的测试明确要求：
  - 返回 `NOT_LEADER`
  - 返回 `leader hint`
  - 保持 retryable 语义
- 这意味着 follower / non-authority 节点只能拒绝或重定向，不能直接接受 dynamic Metadata join。

### 验证命令和结果

- 构建命令：
  - `(
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario
    ) 9>/tmp/cqupt_raft_build.lock`
  - 结果：PASS

- 建议的 CTest 命令：
  - `ctest --preset debug-tests -R "MetadataClientScenario" --output-on-failure`
  - 结果：未用于最终判定
  - 原因：当前环境中 `ctest` 在加载测试时被无关目标 `test_integrated_object_storage_quorum` 的 PRE_TEST discovery 权限错误拦截，错误与 T056 新增测试无关。

- 实际执行的定向测试命令：
  - `./build/linux/tests/test_metadata_client_scenario --gtest_filter='MetadataClientScenarioTest.*'`
  - 结果：PASS

- 日志：
  - `tmp/test-logs/t056-build.log`
  - `tmp/test-logs/t056-ctest.log`
  - `tmp/test-logs/t056-direct.log`

### 结果

- 状态：PASS
- 已在 `tasks.md` 中只勾选 T056 完成。
- 可以进入后续任务。
- 当前仍未实现 JoinMetadataCluster / AddLearner 生产契约；该缺口由后续 T059/T060 继续落地。
