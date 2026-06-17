T100 为 surviving ViewNode failover 增加了独立回归覆盖，新增 `tests/view_failover_test.cpp`，并在 `tests/CMakeLists.txt` 注册 `view_failover_test` 目标。

新增 2 个用例：
- `ViewFailoverTest.SurvivingViewNodeRemainsAvailableWhenFailoverLeavesPartialRegistry`
- `ViewFailoverTest.SurvivingViewNodeCanStayDegradedWithoutBecomingUnavailable`

覆盖点：
- `view-1` 下线、`view-2` 存活时，`GetClusterView()` 仍返回 `kOk`
- surviving ViewNode 仍保持 `liveness=live`
- surviving ViewNode 可表达 `healthy` 或 `degraded`，但不传播为 `unavailable`
- `DiscoverMetadata()` 在 failover 后仍返回可用 metadata
- `DiscoverStorage()` 在 partial registry 场景下返回 `kNotFound` 而不是 `kServiceUnavailable`
- cluster degraded / partial 不会被误传播为 node unavailable
- 现有 `ViewFailoverScriptValidation` 与 `ViewNodeDiscoveryTest.IntegrationFailoverDiscoveryUsesSurvivorObservedRegistryState` 语义保持不退化

验证：
- 构建：`cmake --build --preset debug-ninja-low-parallel --target view_failover_test`
- 测试：`ctest --preset debug-tests -R "ViewFailover|FailoverView|ViewNode" --output-on-failure`

结果：PASS
