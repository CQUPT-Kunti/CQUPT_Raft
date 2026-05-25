# T002 RaftNode 状态机装配调查

## 1. T002 结论

- `RaftNode` 通过 `std::unique_ptr<IStateMachine> state_machine_` 持有业务状态机。
- 当前默认装配不是纯 `KvStateMachine`，也不是纯 `StrongConsistencyMetadataStateMachine`，而是 `CompositeKvMetadataStateMachine`。
- 默认 app/demo/test 基本都走 `RaftNode(config)` 或 `RaftNode(config, snapshotConfig)`，因此隐式依赖的都是 composite。
- 当前仓库未发现外部显式向 `RaftNode` 注入 `std::unique_ptr<KvStateMachine>` 或 `std::unique_ptr<StrongConsistencyMetadataStateMachine>` 的实际使用点。
- 当前 snapshot/restart 的关键问题是：`CompositeKvMetadataStateMachine::SaveSnapshot()` / `LoadSnapshot()` 只委托给 `KvStateMachine`，默认恢复路径仍是 KV 主导。
- 后续 metadata-only 替换的核心装配点已经明确：默认构造函数、`InitServer()`、`Describe()`、`DebugGetValue()`、`GetMetadataStateMachine()`、以及所有依赖默认 `RaftNode` 构造的测试 fixture。

## 2. 已读取的 AGENTS.md

- 根 `AGENTS.md`
- `modules/raft/node/AGENTS.md`
- `modules/raft/state_machine/AGENTS.md`
- `modules/raft/service/AGENTS.md`
- `modules/raft/storage/AGENTS.md`
- `apps/AGENTS.md`
- 同时参考：
  - `specs/006-remove-kv-metadata-state-machine/plan.md`
  - `specs/006-remove-kv-metadata-state-machine/tasks.md`

## 3. RaftNode 如何持有 StateMachine

- 接口定义：`modules/raft/state_machine/state_machine.h`
- 核心接口：
  - `Apply(index, command_data)`
  - `SaveSnapshot(file_path)`
  - `LoadSnapshot(file_path)`
- 持有成员：`modules/raft/node/raft_node.h`
  - `std::unique_ptr<IStateMachine> state_machine_`
- 构造重载：
  - `RaftNode(NodeConfig config)`
  - `RaftNode(NodeConfig config, snapshotConfig snapshot_config)`
  - `RaftNode(NodeConfig config, std::unique_ptr<IStateMachine> state_machine)`
  - `RaftNode(NodeConfig config, snapshotConfig snapshot_config, std::unique_ptr<IStateMachine> state_machine)`

## 4. KVStateMachine 在哪里实例化

- 类定义：`modules/raft/state_machine/state_machine.h/.cpp`
- 运行时默认实例化点：
  - `modules/raft/node/raft_node.h`
  - `CompositeKvMetadataStateMachine` 成员 `KvStateMachine kv_`
- 测试直接实例化点：
  - `tests/test_state_machine.cpp`
  - `tests/test_raft_split_brain.cpp`
- 结论：
  - 默认节点不是直接 `new KvStateMachine`
  - 而是通过 `CompositeKvMetadataStateMachine` 内嵌一个 `KvStateMachine`

## 5. KVStateMachine 在哪里注入 RaftNode

- 默认注入点：`modules/raft/node/raft_node.cpp`
  - `RaftNode(NodeConfig config)` -> `std::make_unique<CompositeKvMetadataStateMachine>()`
  - `RaftNode(NodeConfig config, snapshotConfig snapshot_config)` -> `std::make_unique<CompositeKvMetadataStateMachine>()`
- 自定义注入接口虽然存在，但本次检索未发现实际调用点：
  - 没有发现 `std::make_shared<RaftNode>(..., std::make_unique<...StateMachine>())`
  - 没有发现 `std::make_unique<KvStateMachine>()` 传入 `RaftNode`

## 6. CompositeKvMetadataStateMachine 的真实行为

- `Apply()`：
  - metadata 命令 -> `metadata_.Apply(...)`
  - 其他命令 -> `kv_.Apply(...)`
- `GetValue()` / `DebugString()`：
  - 只面向 `kv_`
- `MetadataStateMachine()`：
  - 返回 `metadata_` 指针
- `SaveSnapshot()`：
  - 只调用 `kv_.SaveSnapshot(file_path)`
- `LoadSnapshot()`：
  - 只调用 `kv_.LoadSnapshot(file_path)`
- 关键结论：
  - 默认节点虽然带 metadata 子状态机
  - 但默认 snapshot/restart 恢复面并不是 metadata snapshot

## 7. 测试 helper / fixture 如何创建 RaftNode + 状态机

- 本次未发现独立的通用 state-machine factory helper。
- 绝大多数 fixture / runner / cluster helper 直接使用：
  - `std::make_shared<RaftNode>(config)`
  - `std::make_shared<RaftNode>(config, snapshot_config)`
- 典型文件：
  - `tests/test_kv_service.cpp`
  - `tests/test_raft_log_replication.cpp`
  - `tests/test_raft_commit_apply.cpp`
  - `tests/raft_integration_test.cpp`
  - `tests/snapshot_test.cpp`
  - `tests/persistence_test.cpp`
  - `tests/test_raft_snapshot_catchup.cpp`
  - `tests/test_raft_snapshot_restart.cpp`
  - `tests/test_raft_snapshot_diagnosis.cpp`
  - `tests/test_raft_split_brain.cpp`
  - `tests/test_raft_replicator_behavior.cpp`
  - `tests/test_t017_leader_switch_ordering.cpp`
  - `tests/metadata_failover_test.cpp`
- 结论：
  - 大量测试没有显式写 `KvStateMachine`
  - 但因为它们走默认 `RaftNode` 构造，所以仍然依赖 KV wiring

## 8. main / demo / service 如何绑定 KVService、KVStateMachine、RaftNode

- `apps/main.cpp`
  - 只构造 `NodeConfig` / `snapshotConfig`
  - 然后执行 `std::make_shared<RaftNode>(node_config, snapshot_config)`
- `RaftNode::InitServer()`
  - 创建并注册：
    - `RaftServiceImpl`
    - `KvServiceImpl`
    - `MetadataServiceImpl`
- `KvServiceImpl`
  - `Put/Delete` -> `CommandType::kSet/kDelete` -> `node_.Propose(command)`
  - `Get` -> `node_.DebugGetValue(...)`
- `MetadataServiceImpl`
  - 写请求 -> `node_.ProposeMetadata(...)`
  - 读请求 -> `node_.GetMetadataStateMachine()` -> metadata query

## 9. snapshot / replay / follower catch-up 如何调用 StateMachine

- startup recovery：
  - `LoadLatestSnapshotOnStartup()` -> `state_machine_->LoadSnapshot(...)`
  - 若 `commit_index_ > last_applied_`，继续 `ApplyCommittedEntries()` 回放日志
- follower 安装快照：
  - `OnInstallSnapshot()` 在 `apply_mu_` 保护下调用 `state_machine_->LoadSnapshot(...)`
- committed log apply：
  - `ApplyCommittedEntries()` 逐条调用 `state_machine->Apply(apply_index, command_data)`
- leader / follower 提交推进后：
  - `OnAppendEntries()` 在 follower 提交推进后调用 `ApplyCommittedEntries()`
  - leader proposal 成功推进 commit 后也会调用 `ApplyCommittedEntries()`
- snapshot save：
  - snapshot worker 在 `apply_mu_` 保护下调用 `state_machine_->SaveSnapshot(...)`
  - 随后交给 `snapshot_storage_->SaveSnapshotFile(...)` 发布
- 结论：
  - Raft core 只认 `IStateMachine`
  - 真正决定恢复语义的是当前注入实现
  - 现状默认实现是 composite，而 composite 的快照实现仍是 KV-only

## 10. 后续 MetadataStateMachine 应替换的装配点

- `RaftNode` 默认构造函数中的 `std::make_unique<CompositeKvMetadataStateMachine>()`
- `CompositeKvMetadataStateMachine` 本身
- `RaftNode::InitServer()` 中 `KvServiceImpl` 的创建与注册
- `RaftNode::Describe()` 中 `kv=` 调试摘要
- `RaftNode::DebugGetValue()` 这条 KV 只读旁路
- `RaftNode::GetMetadataStateMachine()` 中对 composite 的兼容分支
- 所有依赖默认 `RaftNode(config[, snapshot])` 的测试 fixture / runner / cluster helper

## 11. 后续需要同步更新的 AGENTS.md

- 根 `AGENTS.md`
- `modules/raft/node/AGENTS.md`
- `modules/raft/state_machine/AGENTS.md`
- `modules/raft/service/AGENTS.md`
- `apps/AGENTS.md`
- `modules/raft/storage/AGENTS.md`

## 12. 风险点

- 当前默认装配的真正业务主模型不是 metadata-only，而是 composite。
- 当前 metadata service 已存在，但默认 snapshot/restart 仍不是 metadata-only 恢复路径。
- 大量测试依赖默认 `RaftNode` 构造；一旦翻转默认装配，会同步影响大批 Raft 回归。
- `DebugGetValue()` 是测试侧最广泛的 KV 隐式耦合点之一。
- `tests/test_raft_split_brain.cpp` 里局部 `KvStateMachine snapshot_state` 说明有测试直接假设安装快照载荷是 KV snapshot 文件。

## 13. 验收结果

- 已生成报告：
  - `specs/006-remove-kv-metadata-state-machine/task-reports/t002-raftnode-state-machine-wiring.md`
- 已说明 `RaftNode` 如何持有 `IStateMachine`
- 已定位 `KvStateMachine` 的实例化点与默认注入点
- 已说明测试 fixture / runner 如何通过默认 `RaftNode` 构造隐式拿到 KV wiring
- 已说明 main/demo/service 如何通过 `RaftNode` 绑定 `KvService` / `MetadataService`
- 已说明 snapshot/replay/follower catch-up 如何调用 `state_machine_->Apply/SaveSnapshot/LoadSnapshot`
- 已明确后续 `MetadataStateMachine` 应替换的装配点
- 本次未修改源码、未删除 KV、未新增状态机、未修改任何 `AGENTS.md`
- 本次未进入 T003
- Linux 结果：仅静态调查，未运行构建/测试
- Windows 结果：仅静态调查，未运行构建/测试
- CTest 结果：未执行
