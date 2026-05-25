# Phase 0 Research: Remove KV Metadata State Machine

## Research Goal

本阶段只回答三个问题，并把答案固化成实现边界：

1. 当前测试和节点装配是怎样实例化 `RaftNode` 的？
2. `KvStateMachine` / `KvService` / `SET|DEL` 是怎样注入主路径的？
3. metadata-only 主路径在当前代码上最小风险的迁移顺序是什么？

## Findings

### Decision: 当前默认节点装配仍是 KV+Metadata 双栈

**Decision**: 视当前 `RaftNode` 默认装配为必须拆除的过渡实现，而不是可长期保留的兼容层。  
**Rationale**:
- `RaftNode(config)` 和 `RaftNode(config, snapshotConfig)` 默认创建 `CompositeKvMetadataStateMachine`，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:134)
- `InitServer()` 同时注册 `KvServiceImpl` 和 `MetadataServiceImpl`，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:388)
- `Describe()` 与 `DebugGetValue()` 仍优先暴露 KV 调试视图，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:548)
**Alternatives considered**:
- 保留 composite 作为长期 fallback：拒绝，违背“no KV fallback”。
- 只删 `KvService` 不删 composite：拒绝，因为测试和调试仍会继续依赖 KV 状态机。

### Decision: 现有 metadata V1 可复用，但必须升级为 V2

**Decision**: 复用现有 metadata 代码和测试作为迁移种子，但不以当前 object-record V1 为最终模型。  
**Rationale**:
- 当前 metadata V1 已经存在 `metadata_command.*`、`metadata_state_machine.*`、`metadata_service_impl.*` 和 `raft_metadata_client.cpp`
- 但状态机内部只有 `records_ / tombstones_ / replay_table_`，且只支持 object `create/commit/delete/head/list`，[metadata_state_machine.h](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/state_machine/metadata_state_machine.h:47)
- feature 需要 bucket、abort、request table、object index、high-concurrency read path 和 snapshot boundary 一致性
**Alternatives considered**:
- 从零重写 metadata 子系统：拒绝，丢弃现有 metadata 测试资产。
- 只增量加几个字段不改结构：拒绝，无法自然表示 bucket/object/request/index 关系。

### Decision: 测试迁移必须先于默认装配翻转

**Decision**: 先搭 metadata test helper 和 metadata 断言，再把 `RaftNode` 默认装配翻转为 metadata-only。  
**Rationale**:
- 大量核心回归测试直接构造默认 `RaftNode`，例如 [test_raft_log_replication.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_log_replication.cpp:158)、[test_raft_commit_apply.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_commit_apply.cpp:158)
- 它们依赖 `CommandType::kSet/kDelete` 和 `DebugGetValue()`，[test_raft_log_replication.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/test_raft_log_replication.cpp:252)、[raft_integration_test.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/tests/raft_integration_test.cpp:395)
- 如果直接翻默认装配，测试会同时失去业务命令与断言通道
**Alternatives considered**:
- 先切默认构造，再集中修测试：拒绝，编译和验证窗口过大。
- 永久保留显式 KV test harness：拒绝，形成 regression-only KV path。

### Decision: 现有 apply 顺序机制可以保留，状态机并发模型需要升级

**Decision**: 保留 `RaftNode::ApplyCommittedEntries()` + `apply_mu_` 的顺序保障，把主要改造集中在 `MetadataStateMachine` 的锁粒度和内部 applied boundary。  
**Rationale**:
- 当前 `Propose()` / `ProposeMetadata()` 复制完成后都走同一 `ApplyCommittedEntries()`，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:1939)
- `SnapshotWorkerLoop()` 已在 `apply_mu_` 保护下调用 `SaveSnapshot()`，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:2676)
- metadata 状态机目前只用 `std::mutex`，不支持 shared read path，也没记录内部 `last_applied_index/term`
**Alternatives considered**:
- 新增独立 apply 队列线程：拒绝，触碰更多 Raft 核心并发路径。
- 仅靠状态机本地 mutex：拒绝，无法满足并发读和 snapshot consistency 目标。

### Decision: SnapshotStorage 的 crash-safe publish 继续复用

**Decision**: snapshot 的 durability、staging、rename、目录同步继续由 `SnapshotStorage` 负责；metadata state machine 只负责生成更强自描述的数据文件。  
**Rationale**:
- `FileSnapshotStorage` 已具备目录级 publish 语义和 cross-platform durability 处理，[snapshot_storage.h](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/storage/snapshot_storage.h:40)
- 当前 metadata state machine 的 `SaveSnapshot()` 只写单文件，不应该复制外层目录发布职责
**Alternatives considered**:
- 在 metadata state machine 自己实现目录 publish：拒绝，重复 durability 逻辑。
- 完全不改 metadata snapshot 格式：拒绝，无法校验 `snapshot meta == metadata data` 的 applied boundary。

### Decision: 命令路径最终要去掉 KV `SET/DEL` 包装

**Decision**: 最终删除 `CommandType::kSet` / `kDelete` 和 `SET|DEL|...` 编码，仅保留 metadata-only 日志载荷与内部 no-op marker。  
**Rationale**:
- 当前 `Command` 仍是 `SET|key|value` / `DEL|key|` / `META|size|payload` 三路混合编码，[command.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/common/command.cpp:37)
- `ProposeMetadata()` 仍先包一层 `CommandType::kMetadata` 再写日志，[raft_node.cpp](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/raft_node.cpp:1965)
- 继续保留 `SET/DEL` 只会延长 KV 残留寿命
**Alternatives considered**:
- 永久保留 `Command` 并只移除测试：拒绝，因为日志与 codec 主路径仍是 KV-era 设计。
- 完全移除所有字符串 envelope：不优先，内部 no-op/snapshot marker 仍需保留一条稳定内核路径。

### Decision: 状态/健康/指标不能跟着 KvService 一起消失

**Decision**: `KvService` 删除后，`Status/Health/Metrics` 必须迁移到 non-KV 管理面。  
**Rationale**:
- 当前这些诊断能力都挂在 `KvService` 上，[proto/raft.proto](/home/yangjilei/Code/C++/CQUPT_Raft/proto/raft.proto:11)
- 删除 KV 的同时丢掉运维诊断不符合 constitution 的 observability 原则
**Alternatives considered**:
- 保留 `KvService` 仅承载诊断：拒绝，因为仍保留 KV service surface。
- 完全删除诊断 RPC：拒绝，因为回归/故障定位会退化。

### Decision: Windows/Linux 测试入口也必须 metadata-only 化

**Decision**: 迁移不仅是代码删除，还包括 `tests/CMakeLists.txt`、`test.sh` 和 `test.ps1` 的主入口替换。  
**Rationale**:
- Linux `test.sh` 仍有 `kv-service` group，[test.sh](/home/yangjilei/Code/C++/CQUPT_Raft/test.sh:43)
- Windows `test.ps1` conservative fallback 仍写明 `KvStateMachineTest` 子集，[test.ps1](/home/yangjilei/Code/C++/CQUPT_Raft/test.ps1:20)
**Alternatives considered**:
- 只改 CTest target 不改脚本：拒绝，因为用户级验证入口仍会宣传 KV 主路径。

## Migration Boundary Conclusions

- 需要保留的不是 KV 业务模型，而是 Raft 核心能力与高价值回归覆盖。
- metadata-only 迁移的首要风险不是服务接口，而是默认装配和测试断言仍绑在 KV 上。
- snapshot/restart/catch-up 的外层 durable publish 机制已经足够强，应复用；真正要升级的是 metadata state machine V2 的数据模型与自描述边界。
- 最终形态允许 public/proto/snapshot business surface 明确 break，但不允许 silent downgrade、隐式兼容或 KV fallback。
