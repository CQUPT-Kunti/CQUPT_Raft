# T023 切换 RaftNode 默认状态机装配到 MetadataStateMachine

## 结论

- T023 已完成。
- `RaftNode` 默认状态机装配已从 `CompositeKvMetadataStateMachine` 切换到 `MetadataStateMachine`。
- 默认 `RaftNode(config)` / `RaftNode(config, snapshotConfig)` 不再隐式创建 KV / composite 状态机。
- KV 代码、`KvService`、`raft_kv_client`、`kv.proto` 仍保留，但已不再是默认状态机主路径。

## 实际修改

- 更新 `modules/raft/node/raft_node.cpp`
  - 默认构造函数：
    - `RaftNode(NodeConfig config)`
    - `RaftNode(NodeConfig config, snapshotConfig snapshot_config)`
  - 默认装配从：
    - `std::make_unique<CompositeKvMetadataStateMachine>()`
  - 切换为：
    - `std::make_unique<MetadataStateMachine>()`
  - 保留 `RaftNode(..., std::unique_ptr<IStateMachine>)` 注入路径不变。
  - 保留 `CompositeKvMetadataStateMachine` 类型本身，作为过渡兼容细节，不再是默认主路径。
  - 新增 `GetMetadataStateMachineV2()` / `const GetMetadataStateMachineV2() const`，用于显式访问默认 `MetadataStateMachine`。
- 更新 `modules/raft/node/raft_node.h`
  - 增加 `MetadataStateMachine` 前置声明。
  - 暴露 `GetMetadataStateMachineV2()` 只读/可写 getter。
- 更新 `tests/metadata_state_machine_test.cpp`
  - 新增 `RaftNodeDefaultStateMachineWiringUsesMetadataStateMachine`
  - 验证：
    - 默认 `RaftNode(config, snapshot)` 持有 `MetadataStateMachine`
    - 旧的 `GetMetadataStateMachine()` 返回 `nullptr`
    - `DebugGetValue()` 不再从默认状态机读取 KV
- 更新 `modules/raft/node/AGENTS.md`
  - 同步默认业务状态机装配应优先指向 `MetadataStateMachine`
  - 标明 `CompositeKvMetadataStateMachine` 只保留为非默认兼容细节

## 默认 wiring 变化

- 变更前
  - 默认 `RaftNode` 隐式创建 `CompositeKvMetadataStateMachine`
  - 默认主路径同时带有 KV 状态机语义
- 变更后
  - 默认 `RaftNode` 隐式创建 `MetadataStateMachine`
  - `RaftNode` 仍只通过 `std::unique_ptr<IStateMachine>` 持有并调用 `Apply / SaveSnapshot / LoadSnapshot`
  - `CompositeKvMetadataStateMachine` 不再参与默认装配

## 仍保留的 KV / 过渡项

- `KvStateMachine`：保留
- `KvService`：保留
- `raft_kv_client`：保留
- `kv.proto`：保留
- `InitServer()` 中仍注册 `KvService` 与旧 `MetadataService`
  - 这是 service 层过渡残留
  - 不代表默认业务状态机仍是 KV
- 旧 record-centric `MetadataServiceImpl` 仍依赖 `GetMetadataStateMachine()`（`StrongConsistencyMetadataStateMachine*`）
  - 当前任务未迁移该 service 逻辑
  - 这是后续 metadata service 迁移点

## Linux 验证

- 选择原因
  - 本次是默认 wiring 变更，影响 `RaftNode`、`raft_demo`、service 编译链和默认构造测试。
  - 因此不能只跑 `MetadataStateMachine` 单测。
  - 采用最小必要验证：
    - configure
    - 构建 `raft_demo + test_metadata_state_machine + test_raft_split_brain`
    - 跑 `MetadataStateMachineTest`
    - 额外跑一个覆盖默认 `RaftNode(config, snapshot)` 构造的现有 `RaftSplitBrainTest` 过滤

- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target raft_demo test_metadata_state_machine test_raft_split_brain`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^RaftSplitBrainTest\\.StaleAppendEntriesIsRejectedAfterNodeObservesHigherTerm$"`

- 结果
  - `cmake --preset debug-ninja-low-parallel`：PASS
  - `cmake --build --preset debug-ninja-low-parallel --target raft_demo test_metadata_state_machine test_raft_split_brain`：PASS
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`：PASS，`33/33` 通过
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^RaftSplitBrainTest\\.StaleAppendEntriesIsRejectedAfterNodeObservesHigherTerm$"`：PASS，`1/1` 通过

- 日志
  - configure 结果记录：`tmp/test-logs/t023-configure.log`
  - build 日志：`tmp/test-logs/t023-build.log`
  - metadata 单测日志：`tmp/test-logs/t023-metadata-ctest.log`
  - 默认构造 RaftNode 验证日志：`tmp/test-logs/t023-raftnode-ctest.log`

## 未跑全量 CTest 的说明

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 原因：
  - 默认 wiring 变更已经通过 `raft_demo` 构建链、`MetadataStateMachineTest`、以及一个默认 `RaftNode(config, snapshot)` 路径测试做了最小必要验证。
  - 当前仓库仍有大量 KV 断言型 Raft 测试未迁移，不适合在 T023 阶段机械执行全量并把失败混入 wiring 结论。

## 风险与后续迁移点

- 旧 KV 断言型测试仍是后续迁移点
  - 例如大量 `Propose(SetCommand(...))` / `DebugGetValue()` 回归仍未切到 metadata-only 断言
- `MetadataServiceImpl` 仍面向旧 `StrongConsistencyMetadataStateMachine`
  - 默认节点现已不是该状态机
  - 相关 service 行为迁移留待后续任务
- 本次未改 `ApplyCommittedEntries`、snapshot worker、startup replay、follower catch-up 语义
  - 这些路径仍通过 `IStateMachine` 抽象调用

## 验收结果

- `RaftNode` 默认状态机已切换到 `MetadataStateMachine`：已完成
- 默认 `RaftNode` 构造不再隐式创建 `CompositeKvMetadataStateMachine`：已完成
- `RaftNode` 仍只通过 `IStateMachine` 调用 `Apply / SaveSnapshot / LoadSnapshot`：保持成立
- `MetadataStateMachine` 成为默认业务状态机：已完成
- KV 代码仍保留但不再作为默认状态机主路径：已完成
- 相关 `AGENTS.md` 已同步更新默认 wiring 说明：已完成
- Linux 相关构建和必要个别测试通过：已完成
- 未删除 KV 文件：保持成立
- 未实现 `MetadataService` 业务逻辑：保持成立
- 未进入 T024：保持成立

## 说明

- `tasks.md` 当前已有另一条不同含义的 `T023`，本次按用户明确指令执行并单独出具报告，未改 `tasks.md` 标记。
