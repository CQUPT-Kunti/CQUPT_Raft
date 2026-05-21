# T003 CTest/GTest 对 KV 依赖迁移图

## 1. T003 结论

- 当前 CTest/GTest 仍把 KV 当作主验证面的一部分，而不是纯历史残留。
- 依赖分三层：测试目标依赖 `raft_core` 中的 KV 实现、测试脚本依赖 `KvStateMachineTest/RaftKvServiceTest`、测试断言依赖 `CommandType::kSet/kDelete` 与 `DebugGetValue()`。
- 必须迁移的是 Raft 回归测试；可以删除的是纯 KV API/demo 测试；待确认的是 helper、CMake target 混合、proto/RPC 混合、composite 恢复路径测试。

## 2. CMake/CTest 入口影响

- `CMakeLists.txt`: `raft_core` 仍编译 `kv_service_impl.cpp` 与 `state_machine.cpp`，测试 target 默认可见 KV 实现。
- `CMakeLists.txt`: 仍构建 `raft_kv_client`，测试体系仍保留 KV demo 可执行面。
- `tests/CMakeLists.txt`: 仍注册 `test_state_machine`、`test_kv_service`、`test_command`。
- `tests/CMakeLists.txt`: 所有 Raft 回归 target 仍统一链接 `raft_core`，因此隐式带入 composite/KV wiring。
- `test.sh`: `unit` 分组仍包含 `KvStateMachineTest`。
- `test.sh`: 仍保留 `kv-service` 分组，对应 `RaftKvServiceTest`。
- `test.ps1`: Windows fallback subset 仍写死 `KvStateMachineTest`，说明 Windows 默认保守入口仍依赖 KV。

## 3. A. 必须迁移到 MetadataStateMachine

- `tests/test_command.cpp` | 依赖 `CommandType::kSet/kDelete` | 本质是日志载荷编解码基线 | 改为 metadata payload/command wrapper 断言。
- `tests/test_raft_election.cpp` | 依赖 `kSet` 提案辅助 | 本质是 leader election | 保留 Linux+Windows，迁为 metadata write + status 断言。
- `tests/test_raft_log_replication.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是 log replication | 保留 Linux+Windows，迁为 metadata apply/query 一致性。
- `tests/test_raft_commit_apply.cpp` | 依赖 `kSet/kDelete` + `DebugGetValue()` | 本质是 commit/apply 顺序 | 保留 Linux+Windows，迁为 object 生命周期与 ordered apply。
- `tests/raft_integration_test.cpp` | 依赖 `kSet/kDelete` + `DebugGetValue()` | 本质是多节点集成复制/恢复 | 保留 Linux+Windows，迁为 metadata cluster read path。
- `tests/test_t017_leader_switch_ordering.cpp` | 依赖 `kSet/kDelete` + `DebugGetValue()` | 本质是 leader change 后提交顺序 | 保留 Linux+Windows，迁为 request_id 幂等 + leader switch。
- `tests/test_raft_split_brain.cpp` | 依赖 `DebugGetValue()` + 局部 `KvStateMachine snapshot_state` | 本质是 split brain / install snapshot / uncommitted 隔离 | 保留 Linux+Windows，重写为 metadata 状态与 snapshot payload 断言。
- `tests/test_raft_replicator_behavior.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是 per-follower replication/backoff | 保留 Linux+Windows，迁为 metadata follower convergence。
- `tests/snapshot_test.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是 snapshot save/load/recovery | 保留 Linux+Windows，恢复后必须校验 `request_table/tombstone/object_index`。
- `tests/persistence_test.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是 restart recovery / persisted boundary | 保留 Linux+Windows，恢复后必须校验 `last_applied_index/term` 与 request 幂等。
- `tests/persistence_more_test.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是补充恢复/边界回归 | 保留 Linux+Windows，迁为 metadata 边界与 stale state 断言。
- `tests/test_raft_snapshot_catchup.cpp` | 依赖 `kSet/kDelete` + `DebugGetValue()` | 本质是 follower catch-up | 保留 Linux+Windows，catch-up 后必须校验 `request_table/tombstone/object_index`。
- `tests/test_raft_snapshot_restart.cpp` | 依赖 `kSet/kDelete` + `DebugGetValue()` | 本质是 snapshot + restart recovery | 保留 Linux+Windows，恢复后必须校验 `request_table/tombstone/object_index/last_applied_index/term`。
- `tests/test_raft_snapshot_diagnosis.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是 snapshot diagnosis / trusted-state | Linux 重点保留，metadata 恢复断言必须覆盖 snapshot 边界一致性。
- `tests/test_raft_segment_storage.cpp` | 依赖 `kSet` + `DebugGetValue()` | 本质是 segment log + recovery boundary | 保留 Linux+Windows，迁为 metadata replay 与 committed facts。

## 4. B. 可以删除

- `tests/test_state_machine.cpp / KvStateMachineTest` | 直接依赖 `KvStateMachine` | 纯 KV 状态机单测 | 删除，不迁移。
- `tests/test_kv_service.cpp / RaftKvServiceTest` | 直接依赖 `KvService` RPC | 纯 KV API/demo/service 路径 | 删除，由 MetadataService e2e 替代。
- `test.sh / kv-service` | 依赖 `RaftKvServiceTest` | 纯 KV 分组入口 | 删除。
- `test.ps1 / KvStateMachineTest fallback` | 依赖 Windows conservative fallback | 纯 KV 保守入口 | 删除并换成 metadata-only fallback。
- `apps/raft_kv_client.cpp` | 依赖 `KvService::Stub` | 纯 KV demo client | 删除，不保留回归价值。

## 5. C. 待确认

- `tests/test_command.cpp` 的最终归属 | 当前同时覆盖 KV 与 metadata wrapper | 待确认是保留同名 target 还是拆分 metadata-only codec 测试。
- `tests/test_raft_split_brain.cpp` 的安装快照构造 | 当前写死 `KvStateMachine snapshot_state` | 待确认是否改为 metadata snapshot builder helper。
- `tests/CMakeLists.txt` 的 target 组织 | 当前 `test_command/test_state_machine/test_kv_service` 与 metadata targets 并列 | 待确认是否维持 target 名称还是仅替换源文件/标签。
- `CMakeLists.txt / raft_core` | 当前 KV service/state_machine 与 metadata 一起编进核心库 | 待确认先翻默认装配还是先拆 target 源文件。
- `proto/raft.proto` 相关测试入口 | 当前 `KvService` 与 `MetadataService` 并存 | 待确认 admin/status 类回归挂到哪个 non-KV service。
- `CompositeKvMetadataStateMachine` 恢复路径相关测试 | 当前默认 snapshot save/load 只委托给 KV | 待确认迁移时哪些诊断测试需要先重写 fixture。

## 6. Linux + Windows 双平台必须保留

- `test_raft_election.cpp`
- `test_raft_log_replication.cpp`
- `test_raft_commit_apply.cpp`
- `raft_integration_test.cpp`
- `test_t017_leader_switch_ordering.cpp`
- `test_raft_split_brain.cpp`
- `test_raft_replicator_behavior.cpp`
- `snapshot_test.cpp`
- `persistence_test.cpp`
- `persistence_more_test.cpp`
- `test_raft_snapshot_catchup.cpp`
- `test_raft_snapshot_restart.cpp`
- `test_raft_segment_storage.cpp`

## 7. 恢复类测试后续必须补的 metadata 事实

- `snapshot_test.cpp`: 恢复后不能只看对象可见性，必须校验 `request_table`、`tombstone`、`object_index`。
- `persistence_test.cpp`: restart 后必须校验 `request_table` 幂等、`last_applied_index/last_applied_term` 边界。
- `persistence_more_test.cpp`: 必须覆盖 stale `ObjectRecord` / stale `object_index` 不可见。
- `test_raft_snapshot_catchup.cpp`: catch-up 后必须校验 leader/follower 的 `request_table/tombstone/object_index` 一致性。
- `test_raft_snapshot_restart.cpp`: 必须覆盖 `LoadSnapshot + Replay` 后 `request_table/tombstone/object_index/last_applied_index/term` 全量一致性。
- `test_raft_snapshot_diagnosis.cpp`: 必须覆盖 snapshot boundary mismatch、metadata divergence、replay boundary 不重复 apply。
- `test_raft_segment_storage.cpp`: 必须覆盖 log replay 不跳过 committed `MetadataCommand`、不重复 apply snapshot 内条目。

## 8. 后续需要更新的 AGENTS.md

- 根 `AGENTS.md`
- `modules/raft/service/AGENTS.md`
- `modules/raft/state_machine/AGENTS.md`
- `modules/raft/node/AGENTS.md`
- `apps/AGENTS.md`
- `proto/AGENTS.md`

## 9. 验收结果

- 已生成报告：`specs/006-remove-kv-metadata-state-machine/task-reports/t003-kv-test-migration-map.md`
- 已区分 A 必须迁移、B 可以删除、C 待确认。
- 已记录 CMake/CTest 入口影响、AGENTS.md 更新点、双平台保留范围、恢复类 metadata 硬约束。
- 本次仅静态调查；未修改源码、测试、CMake、proto、AGENTS、spec、plan、tasks；未进入 T004。
