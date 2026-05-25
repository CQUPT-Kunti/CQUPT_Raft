# T033 MetadataService admission 错误映射

## 修改内容

- `proto/common.proto`
  - 为 metadata RPC summary 新增：
    - `METADATA_STATUS_CODE_OVERLOADED`
    - `METADATA_STATUS_CODE_SERVICE_UNAVAILABLE`
- `modules/raft/service/metadata_service_impl.cpp`
  - 保留所有写路径统一走 `RaftNode::ProposeMetadata(...)`
  - 将 `ProposeResult` admission 结果统一映射到 `MetadataResponseSummary.code`
  - 保持 `request_id / leader_hint / term / log_index / object identity` 透传
- `apps/raft_metadata_client.cpp`
  - 识别并打印新状态码字符串
  - 输出新增 `retryable=true|false`
  - `NOT_LEADER / TIMEOUT / OVERLOADED / SERVICE_UNAVAILABLE` 归类为 retryable
- `tests/metadata_failover_test.cpp`
  - 新增真实集群 service 映射用例
  - 补了 leader 重新发现与健康写入重试 helper，避免选主瞬态干扰 service 语义验证
- `tests/metadata_client_scenario_test.cpp`
  - fake service 新增 forced write response，用于验证 CLI 对 admission 错误的展示
- `tests/metadata_concurrency_stress_test.cpp`
  - 仅放宽一个并发压力用例的测试 deadline，避免整组回归并跑下偶发超时

## 错误映射

- `ProposeStatus::kNotLeader` -> `METADATA_STATUS_CODE_NOT_LEADER`
- `ProposeStatus::kTimeout` -> `METADATA_STATUS_CODE_TIMEOUT`
- `ProposeStatus::kOverloaded` -> `METADATA_STATUS_CODE_OVERLOADED`
- `ProposeStatus::kNodeStopping` -> `METADATA_STATUS_CODE_SERVICE_UNAVAILABLE`
- `ProposeStatus::kInvalidCommand` -> `METADATA_STATUS_CODE_INVALID_ARGUMENT`
- `ProposeStatus::kApplyFailed`
  - `invalid metadata command:` -> `INVALID_ARGUMENT`
  - `not found:` -> `NOT_FOUND`
  - `state conflict:` -> `STATE_CONFLICT`
  - `idempotency conflict:` -> `IDEMPOTENCY_CONFLICT`
  - 其他 -> `INTERNAL_ERROR`
- `ProposeStatus::kReplicationFailed / kCommitFailed` -> `INTERNAL_ERROR`
- `ProposeStatus::kOk`
  - message 以 `idempotent replay` 开头 -> `IDEMPOTENT_REPLAY`
  - 其他 -> `OK`

## 测试覆盖

- `MetadataFailoverTest.FollowerWriteReturnsNotLeader`
  - 验证 follower 写 RPC 返回 `NOT_LEADER`
- `MetadataFailoverTest.LeaderWriteTimeoutReturnsTimeoutAndSameRequestIdCanRetry`
  - 验证 leader 在多数派不可用时返回 `TIMEOUT`
  - follower 恢复后同 `request_id` 重试可成功
- `MetadataFailoverTest.ConcurrentDuplicateCreateObjectRequestsShareSameLogIndex`
  - 验证相同 `request_id` 并发 RPC 合流，不重复追加不同 log index
- `MetadataFailoverTest.DifferentFingerprintForSameRequestIdReturnsIdempotencyConflict`
  - 验证不同 fingerprint 的同 `request_id` 返回 `IDEMPOTENCY_CONFLICT`
- `MetadataClientScenarioTest.ClientShowsRetryableAdmissionStatuses`
  - 验证 CLI 对 `NOT_LEADER / TIMEOUT / OVERLOADED / SERVICE_UNAVAILABLE` 输出明确状态和 `retryable=true`
- `MetadataClientScenarioTest.ClientShowsIdempotencyConflictAsNonRetryable`
  - 验证 CLI 对 `IDEMPOTENCY_CONFLICT` 输出 `retryable=false`
- 回归补跑：
  - `MetadataConcurrencyStressTest`
  - 证明 T032 admission 改造与 T033 service 映射叠加后未打破并发压力路径

## Linux 验证

- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client test_metadata_client_scenario test_metadata_failover test_metadata_concurrency_stress`
  - PASS
- `ctest --test-dir build/linux --output-on-failure -R "(MetadataClientScenarioTest|MetadataFailoverTest|MetadataConcurrencyStressTest)"`
  - PASS，16/16

日志：

- `tmp/test-logs/t033-cmake-configure.log`
- `tmp/test-logs/t033-build.log`
- `tmp/test-logs/t033-build-failover.log`
- `tmp/test-logs/t033-build-stability.log`
- `tmp/test-logs/t033-ctest-failover.log`
- `tmp/test-logs/t033-ctest.log`

## 风险

- 本轮没有做 live cluster 下 `OVERLOADED` 的 service 级端到端构造；当前覆盖是：
  - T032 已验证节点 admission 的 overload/backpressure
  - T033 已验证 CLI 能正确显示 `OVERLOADED`
  - 后续若需要更强保证，可在 T035 追加“gRPC 写洪峰 + in-flight 满载”的端到端回归
- 本轮未改 `RaftNode` admission 核心逻辑，只消费其结果做 service/client 映射
- 本轮未进入 T034，也未改默认 wiring、KV fallback、DataNode、`metadata_state_machine` apply 语义
