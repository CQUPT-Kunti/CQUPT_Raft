## T034 MetadataService 读路径 admission 收敛

### 修改文件
- `modules/raft/service/metadata_service_impl.cpp`
- `tests/metadata_failover_test.cpp`
- `tests/metadata_client_scenario_test.cpp`

### 本轮收敛结论
- `HeadObject` / `ListObjects` 现在统一先走 MetadataService 的 read admission，再进入本地 metadata 查询。
- 读路径仍是 leader 本地读，不进入普通 Raft 写日志，也没有新增 KV fallback。
- follower 不再返回本地 stale read 结果，而是明确返回 `NOT_LEADER`。

### Head/List read admission
- admission 顺序固定为：
  1. gRPC deadline 已过：返回 `TIMEOUT`
  2. 节点已停止或正在停止：返回 `SERVICE_UNAVAILABLE`
  3. 当前节点不是 leader：返回 `NOT_LEADER`
  4. 请求字段非法：返回 `INVALID_ARGUMENT`
  5. metadata state machine 未配置：返回 `INTERNAL_ERROR`
  6. admission 通过后才执行 `HeadObject` / `ListObjects` 本地查询
- `HeadObject` 新增请求校验：`bucket`、`object_key` 不能为空。
- `ListObjects` 新增请求校验：`bucket` 不能为空。

### 错误码映射
- follower `HeadObject` / `ListObjects`：`NOT_LEADER`
- stopped 节点命中 service admission：`SERVICE_UNAVAILABLE`
- deadline 已过：`TIMEOUT`
- 非法请求：`INVALID_ARGUMENT`
- bucket/object 不存在：保持原查询语义，返回 `NOT_FOUND`
- metadata state machine 缺失：`INTERNAL_ERROR`

### 当前读一致性边界
- 当前实现只保证“非 leader 不读”，避免了 follower 本地 stale read。
- 当前实现仍然不是正式的线性一致读，因为没有实现 `ReadIndex` 或 leader lease。
- leader 角色刚切换、网络分区或 lease 未定义场景下，`HeadObject` / `ListObjects` 仍只能视为“leader-local read”。
- 因此本轮只固定 admission 和错误边界，不宣称已经提供线性一致读保证。

### 测试覆盖
- `MetadataFailoverTest.FollowerHeadAndListReturnNotLeader`
  - 验证 follower `HeadObject` / `ListObjects` 返回 `NOT_LEADER`
- `MetadataFailoverTest.LeaderHeadAndListInvalidRequestReturnInvalidArgument`
  - 验证 leader 读路径非法请求返回 `INVALID_ARGUMENT`
- `MetadataClientScenarioTest.ReadCommandsShowRetryableAdmissionStatuses`
  - 验证 CLI 对 `NOT_LEADER` / `TIMEOUT` / `SERVICE_UNAVAILABLE` 的展示不再混成普通失败
- 复用已有测试覆盖：
  - leader 正常 `HeadObject` / `ListObjects` 可见 committed object
  - deleted object 不暴露，查询返回 `NOT_FOUND`

### Linux 验证
- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client test_metadata_client_scenario test_metadata_failover test_metadata_state_machine`：PASS
- `ctest --test-dir build/linux --output-on-failure -R "(MetadataClientScenarioTest|MetadataFailoverTest|MetadataStateMachineTest)"`：PASS
- CTest 结果：`46/46` 通过

### Windows 结果
- Windows 未执行，原因是当前环境为 Linux；T034 的 Windows 覆盖将在 T035 统一验证

### 风险
- 未实现 `ReadIndex` / leader lease：仍存在“leader-local read 不是严格线性一致读”的边界风险，需要在 T035 明确保留。
- stopped 节点如果在 gRPC 传输层就已经不可达，调用方可能先看到 transport failure，而不是 service 内部 `SERVICE_UNAVAILABLE`；这是当前 RPC 生命周期边界，不属于本轮 admission 逻辑缺失。
- 本轮没有改 `RaftNode` 默认 wiring，没有改 `MetadataStateMachine` 查询语义，也没有进入 T035。
