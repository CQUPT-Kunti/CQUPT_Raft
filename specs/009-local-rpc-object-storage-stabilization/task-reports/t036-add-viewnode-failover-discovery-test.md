## T036

### 做了什么

- 在 `tests/view_node_discovery_test.cpp` 新增双 ViewNode failover discovery 集成测试。
- 为测试夹具 `RunningViewNodeDiscoveryService` 增加测试专用 `Stop()`，用于模拟单个 ViewNode 停止服务。
- 通过分别向两个 ViewNode registry 注入相同 observed state，建立 failover 基线；未实现生产 peer sync。

### 修改文件

- `tests/view_node_discovery_test.cpp`

### 新增测试

- `ViewNodeDiscoveryTest.IntegrationFailoverDiscoveryUsesSurvivorObservedRegistryState`

### 测试覆盖语义

- 构造两个 ViewNode discovery 入口：`primary` 和 `survivor`。
- 在两个入口上都注册同一份 ViewNode / MetadataNode / StorageNode observed facts，模拟两个 ViewNode 都已经掌握当前 cluster view。
- 对 `survivor` 执行 self refresh，保留其当前 incarnation/sequence/live 状态。
- 停止 `primary` 服务，并推进 `survivor` 的可控时间源，让 `primary` 在 `survivor` 视角下按 TTL 转为 `DEAD`。
- 通过 `survivor` 继续执行 `DiscoverMetadata`、`DiscoverStorage`、`GetClusterView`：
  - Metadata 和 Storage 观测结果仍可发现。
  - failed ViewNode 不会被伪装成健康，cluster view 中可观察到其 `DEAD` liveness。
  - 观测结果仍然只是 observed registry state，不改变 Metadata membership authority，也不涉及 Raft membership 变更。

### local RPC status smoke

- 未执行。
- 原因：T036 只要求测试先行；当前 targeted `test_view_node_discovery` 已覆盖本任务需要的 failover discovery 语义。

### 验证命令和结果

- 构建：
  - `(
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
    ) 9>/tmp/cqupt_raft_build.lock`
  - 结果：PASS
- 测试：
  - `ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure`
  - 结果：PASS，`23/23` 通过，新增测试通过。

### 结果

- 状态：PASS
- 已满足勾选条件，可将 `tasks.md` 中 `T036` 从 `[ ]` 改为 `[x]`。
- 可以进入后续任务；真实 peer sync/network failover 生产能力仍由后续 T039-T041 补齐。
