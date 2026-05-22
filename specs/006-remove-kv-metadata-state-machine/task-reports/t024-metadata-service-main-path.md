# T024 MetadataService 工业化主路径基础能力

## 结论

- T024 已完成。
- `MetadataService` 已接入 `MetadataStateMachine` 主路径。
- 写请求不再走 KV，也不直接改状态机；统一通过 `MetadataCommand + RaftNode::ProposeMetadata(...)` 进入 Raft 提交/应用链路。
- 读请求 `HeadObject / ListObjects` 已走 `MetadataStateMachine` 本地查询路径。
- `KvService` / `kv.proto` / `raft_kv_client` 仍保留为过渡残留，但本次没有作为 metadata fallback。

## 实际修改

- `proto/common.proto`
  - 收敛 metadata 公共契约到 `MetadataStatusCode`、`MetadataObjectState`、`ChunkRef`、`BucketRecord`、`ObjectRecord`、`MetadataResponseSummary`。
- `proto/metadata.proto`
  - 将旧的 record-centric RPC 替换为 bucket/object 生命周期 RPC：
    - `CreateBucket` / `DeleteBucket`
    - `CreateObject` / `CommitObject` / `AbortObject` / `DeleteObject`
    - `HeadObject` / `ListObjects`
- `modules/raft/service/metadata_service_impl.h/.cpp`
  - 重写服务适配层。
  - 写路径：RPC -> `MetadataCommand` -> `SerializeMetadataCommand(...)` -> `RaftNode::ProposeMetadata(...)` -> `MetadataStateMachine::Apply(...)`
  - 读路径：leader 检查 -> `RaftNode::GetMetadataStateMachineV2()` -> `HeadObject(...)` / `ListObjects(...)`
  - `request_id` 已透传到 `MetadataCommand.request_id`
  - 写请求 `client_time_unix_ms` 为空时使用稳定派生时间，避免同 `request_id` 重试触发 fingerprint 漂移
  - 错误码映射已明确：
    - `InvalidCommand` -> `INVALID_ARGUMENT`
    - `ApplyFailed + not found` -> `NOT_FOUND`
    - `ApplyFailed + state conflict` -> `STATE_CONFLICT`
    - `ApplyFailed + idempotency conflict` -> `IDEMPOTENCY_CONFLICT`
    - `NotLeader` -> `NOT_LEADER`
    - `Timeout` -> `TIMEOUT`
    - 其他失败 -> `INTERNAL_ERROR`
- `apps/raft_metadata_client.cpp`
  - 切到 bucket/object CLI：
    - `create-bucket` / `delete-bucket`
    - `create-object` / `commit-object` / `abort-object` / `delete-object`
    - `head-object` / `list-objects`
    - `verify-read-after-write`
  - 输出保留 `request_id`、leader hint、status code、log index 等排障信息。
- `tests/metadata_client_scenario_test.cpp`
  - Fake metadata server 与 CLI 场景测试切到新 RPC。
- `tests/metadata_failover_test.cpp`
  - 集群 failover 测试切到 bucket/object 新 RPC。
- `modules/raft/state_machine/metadata_state_machine.cpp`
  - 补了 Raft 内部 no-op 兼容分支，允许默认 metadata 状态机承接 leader no-op entry。
- `tests/metadata_state_machine_test.cpp`
  - 收紧并发测试断言，避免查询后再次独立读内部索引造成 TOCTOU 假阳性。
- `proto/AGENTS.md` / `modules/raft/service/AGENTS.md` / `apps/AGENTS.md`
  - 同步主路径、target 边界、无 KV fallback 说明。

## 主路径说明

- 写路径如何进入 Raft
  - `MetadataServiceImpl` 为每个写 RPC 构造对应 `MetadataCommand`
  - 通过 `SerializeMetadataCommand(...)` 序列化
  - 调用 `RaftNode::ProposeMetadata(...)`
  - 由 `RaftNode` 复制、提交并调用默认 `MetadataStateMachine::Apply(...)`
- 读路径如何查询 `MetadataStateMachine`
  - `MetadataServiceImpl` 先做 leader 检查
  - 然后通过 `RaftNode::GetMetadataStateMachineV2()` 直接调用 `HeadObject(...)` / `ListObjects(...)`
  - 读路径不写 Raft Log
- `request_id` 如何透传
  - RPC `request_id` 直接写入 `MetadataCommand.request_id`
  - 由 `MetadataStateMachine` 的 `requests_ / request_fingerprints_` 触发幂等与冲突语义
- KV 过渡残留位置
  - `kv.proto`
  - `KvService`
  - `raft_kv_client`
  - 当前仅保留文件和 target；`MetadataService` 没有回退到这些路径

## 当前读一致性边界

- 当前 `HeadObject / ListObjects` 是 leader 本地读。
- 当前未实现 `ReadIndex` / leader lease。
- 因此它不是严格证明过的线性一致读；如果节点短暂自认为 leader 但已经失去多数派，存在读到旧状态的风险。
- 后续需要通过 `ReadIndex` 或 leader lease 收敛该风险。

## Linux 验证

- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client test_metadata_client_scenario test_metadata_failover test_metadata_state_machine`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^(MetadataStateMachineTest|MetadataFailoverTest|MetadataClientScenarioTest)\."`
  - PASS
  - `MetadataStateMachineTest`：`33/33`
  - `MetadataFailoverTest + MetadataClientScenarioTest` 合并后总计 `40/40`

## 日志

- `tmp/test-logs/t024-configure.log`
- `tmp/test-logs/t024-build.log`
- `tmp/test-logs/t024-sm-ctest.log`
- `tmp/test-logs/t024-service-ctest.log`
- `tmp/test-logs/t024-ctest.log`

## 范围与风险

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 未删除 KV 文件。
- 未实现 `DataNode`。
- 未实现 `ReadIndex` / leader lease。
- 未进入 T025。
