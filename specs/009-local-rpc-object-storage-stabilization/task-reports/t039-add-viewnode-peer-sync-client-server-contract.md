## T039

### 做了什么

- 在 `proto/view.proto` 新增 ViewNode peer sync RPC contract：
  - `PullPeerViewSnapshot`
  - `PushPeerViewSnapshot`
- 在 `modules/view/view_service_impl.h/.cpp` 新增对应 server adapter。
- 在 `modules/view/view_client.h/.cpp` 新增对应 client adapter、请求/结果类型和 `peer_sync_timeout`。
- 在 `tests/view_node_discovery_test.cpp` 新增 RPC adapter 集成测试，覆盖：
  - peer snapshot pull/push 走通
  - observed state 经 adapter 导入后可被 discovery/cluster view 观察
  - 旧 incarnation 经 peer sync RPC 仍不能覆盖较新的本地状态

### 修改文件

- `proto/view.proto`
- `modules/view/view_service_impl.h`
- `modules/view/view_service_impl.cpp`
- `modules/view/view_client.h`
- `modules/view/view_client.cpp`
- `tests/view_node_discovery_test.cpp`

### peer sync RPC / adapter 边界

- `PullPeerViewSnapshot` 从本地 registry 导出 observed cluster snapshot，包含：
  - view / metadata / storage snapshots
  - incarnation / sequence / last_seen 等 merge ordering 所需事实
  - leader hint 等 observed metadata facts
- `PushPeerViewSnapshot` 把 peer snapshot 在 service adapter 内部 replay 到现有 registry 边界：
  - 先 `RegisterNode`
  - 再按需要 `HeartbeatNode`
  - 复用现有 incarnation / sequence / stale ordering 语义
- 当前不实现 background peer sync loop，不实现 active-active 调度，不实现 failover client 策略。

### observed-state only 保证

- proto 注释和 adapter 诊断都明确 peer sync 只交换 observed registry state。
- pull/push 响应都会附带 `non_authority_boundary` 诊断，强调：
  - 不改变 Raft membership
  - 不决定 voter / learner
  - 不让 StorageNode registration 进入 Raft log
  - 不改变 committed object visibility
- metadata membership_state 仍只是 observed facts；adapter 不把它升级成 authority。

### 新增/更新测试

- `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcExportsAndImportsObservedState`
- `ViewNodeDiscoveryTest.IntegrationPeerSyncRpcOldIncarnationCannotOverrideNewerState`

### 验证命令与结果

- 构建命令：
  - `(
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery view_node_app
    ) 9>/tmp/cqupt_raft_build.lock`
- 结果：PASS
- 测试命令：
  - `ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure`
- 实际结果：
  - targeted build 通过
  - `ViewNodeDiscovery` 测试 `26/26` 通过

### 状态

- 状态：PASS
- 已满足勾选条件，可将 `tasks.md` 中 T039 从 `[ ]` 改为 `[x]`

### 后续

- 可以进入 T040，继续实现 Push/Pull 后的 registry merge 与复用路径。
