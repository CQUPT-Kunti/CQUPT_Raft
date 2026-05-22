# T032 metadata proposal admission

## 修改文件

- `modules/raft/common/propose.h`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `tests/metadata_concurrency_stress_test.cpp`
- `tests/CMakeLists.txt`
- `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`
- `specs/006-remove-kv-metadata-state-machine/task-reports/T032-metadata-proposal-admission.md`

## admission 入口说明

- `RaftNode::ProposeMetadata()` 现在作为 metadata 写请求的统一 admission 入口。
- 进入 Raft proposal 前先做：
  - metadata command parse/validate
  - `running_` 检查
  - `role_ == Leader` 检查
  - caller deadline 是否已过期检查
  - `request_id + fingerprint` 幂等冲突检查
- admission 失败不再静默丢弃，明确返回：
  - `kNodeStopping`
  - `kNotLeader`
  - `kTimeout`
  - `kApplyFailed`（同 `request_id` 对应不同 command fingerprint）
  - `kOverloaded`

## bounded queue / backpressure 说明

- metadata proposal path 增加了节点内受控 in-flight 限制：
  - `kMaxInflightMetadataProposals = 4`
- 新请求若未命中已完成缓存、也未命中同 `request_id` 的 in-flight tracker，则必须占用一个 in-flight 槽位。
- 当 `metadata_inflight_proposals_` 已满时，直接返回：
  - `ProposeStatus::kOverloaded`
  - message: `metadata proposal admission rejected: in-flight limit reached`
- 同 `request_id` 且 fingerprint 相同的并发重试不会重复入队，而是复用同一个 tracker 等待结果。

## timeout handling 说明

- `ProposeMetadata()` 调用线程只等待到 `config_.rpc_deadline`，超时后返回：
  - `ProposeStatus::kTimeout`
  - message: `timed out waiting for metadata proposal completion`
- 超时不会卡住调用线程；后台 worker 仍可继续完成复制、commit 和 apply。
- `ExecuteMetadataProposal()` 的复制等待使用更长的内部 deadline：
  - `ScaleDeadline(config_.rpc_deadline, 20)`
- 如果日志后来成功提交并 apply，后续相同 `request_id` 重试会：
  - 优先命中 in-flight tracker 或 completed cache
  - 最终仍依赖 `MetadataStateMachine::Apply()` / `request_table` 的幂等语义给出一致结果
- timeout 结果不会伪装成成功，也不会被缓存成 completed result。

## no-double-apply 防护说明

- proposal admission 层为相同 `request_id + fingerprint` 建立单个 `MetadataProposalTracker`，并发相同请求只会共享一个 proposal worker，不会重复 append 多条等价 metadata log。
- 相同 `request_id` 但不同 fingerprint 会被 admission 直接拒绝，避免旧 request_id 映射到新命令。
- worker 成功复制后仍沿用既有 `AdvanceCommitIndexUnlocked() + ApplyCommittedEntries()` 路径，没有绕过 `apply_mu_` 和状态机幂等语义。
- `ApplyCommittedEntries()` 之后额外校验 `last_applied_ >= log_index`，确保返回成功前对应 committed entry 已经完成 apply。
- 本任务没有改动 `MetadataStateMachine::Apply()` 的幂等逻辑，也没有改动 Raft log apply 顺序机制。

## 新增/调整测试

- 新增 target：
  - `test_metadata_concurrency_stress`
- 新增测试文件：
  - `tests/metadata_concurrency_stress_test.cpp`
- 覆盖场景：
  - `AdmissionRejectsWhenInflightLimitIsReached`
  - `TimeoutReturnsWithoutBlockingAndRetryUsesSameInflightProposal`
  - `ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply`
- 现有 metadata 回归未改动源码，只补跑：
  - `RaftCommitApplyTest`
  - `RaftLogReplicationTest`
  - `RaftLeaderSwitchOrderingTest`

## Linux 结果

- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_concurrency_stress test_raft_commit_apply test_raft_log_replication test_t017_leader_switch_ordering`
  - PASS
- 日志：
  - `tmp/test-logs/t032-cmake-configure.log`
  - `tmp/test-logs/t032-build.log`
  - `tmp/test-logs/t032-build-stress.log`

## Windows 结果

- Windows 未执行，原因是当前环境为 Linux；T032 的 Windows 覆盖将在 T035 统一验证

## CTest 结果

- `ctest --test-dir build/linux --output-on-failure -R "MetadataConcurrencyStressTest"`
  - PASS，3/3
  - 日志：`tmp/test-logs/t032-ctest-stress.log`
- `ctest --test-dir build/linux --output-on-failure -R "(MetadataConcurrencyStressTest|RaftCommitApplyTest|RaftLogReplicationTest|LeaderSwitchOrderingTest)"`
  - PASS，9/9
  - 日志：`tmp/test-logs/t032-ctest.log`

## KV removal status

- 本任务未删除 KV 代码
- 本任务未删除旧 KV command
- 本任务未改动 KV removal 范围之外的 proto/service/client 脚本入口

## 是否修改 proto / service / client / scripts

- `proto/raft.proto`：否
- `metadata_service_impl`：否
- `apps/raft_metadata_client.cpp`：否
- `test.sh / test.ps1`：否

## 是否进入 T033

- 否
- 本次只处理节点级 metadata proposal admission / backpressure / timeout / no-double-apply 防护

## 剩余风险

- `metadata_completed_proposals_` 只驻留节点内存，不跨重启持久化；restart 后的重试一致性仍需在后续恢复验收中继续验证。
- 当前 admission 层 completed cache 只缓存 `kOk / kApplyFailed / kInvalidCommand`；若后续需要更细粒度的 overload / timeout 诊断语义，应在 T033 或后续客户端映射任务中统一审视，但不属于本任务范围。
- T031 遗留的“delete 后同名重建与旧 lifecycle 防护”风险仍在，已写入跨任务风险记录。

## 是否新增/更新 cross-task-risk-notes.md

- 是
- 新增 `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`
- 已写入用户指定的 T031 遗留注意，并追加 T032 的 completed cache / restart replay 风险
