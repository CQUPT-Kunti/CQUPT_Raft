# T056 Production KV Residual Cleanup

## 本次修改文件
- `modules/raft/common/command.h`
- `modules/raft/common/command.cpp`
- `modules/raft/common/config.h`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `apps/main.cpp`
- `CMakeLists.txt`
- 删除 `modules/raft/state_machine/state_machine.h`
- 删除 `modules/raft/state_machine/state_machine.cpp`
- 更新 `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`

## 删除的生产 KV 残留
- 删除 `CommandType::kSet` / `CommandType::kDelete`
- 删除 `SET|` / `DEL|` 编解码路径，仅保留 metadata `META|size|payload` 包装
- 删除 `KvStateMachine` 实现文件与 `KVS1` KV snapshot 路径
- 删除 `RaftNode` 中的 `CompositeKvMetadataStateMachine`
- 删除 `RaftNode::DebugGetValue()`、`ValidateKey()`、`ValidateValue()` 以及对应 KV 校验分支
- 删除 `RaftNode` 中 `RpcKind::kKvPut/kKvDelete/kKvGet/kKvStatus/kKvHealth/kKvMetrics`
- 删除 `KvRequestLimits` / `NodeConfig::kv_limits`，改为通用 `ProposalLimits`
- 删除 `apps/main.cpp` 中 `kv_max_*` 配置和日志命名，改为 `proposal_max_command_bytes`

## 保留的非 KV 内部 marker / metadata-only 路径
- 保留 metadata-only `CommandType::kMetadata`
- 保留内部 `__raft_internal_noop__` 与 `snapshot` marker；它们是 Raft 内部 marker，不是 KV 业务路径
- 保留 `MetadataStateMachine`、`MetadataService`、`raft_metadata_client` 作为唯一业务主路径

## CMake / build graph 清理
- `CMakeLists.txt` 中 `raft_core` 已移除 `modules/raft/state_machine/state_machine.cpp`
- `kv_service_impl.*`、`apps/raft_kv_client.cpp`、KV proto 业务面在本轮前已退役，本轮未恢复任何构建引用
- 本轮未修改 `tests/CMakeLists.txt`；测试清理与迁移留给 `T057`

## 未处理的 tests 残留与转交 T057
- 未迁移 `tests/test_command.cpp`、`tests/test_state_machine.cpp`
- 未迁移 snapshot/restart/recovery/catch-up 相关 `SetCommand/DeleteCommand` / `DebugGetValue()` 断言
- 未处理 `tests/support/raft_snapshot_restart_test_utils.h`
- 未处理 `tests/metadata_state_machine_test.cpp` 中 legacy `DebugGetValue()` 断言
- 上述内容已写入 `cross-task-risk-notes.md`，统一转交 `T057`

## Linux 结果
- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client no_kv_surface_audit`：PASS

## Windows 结果
- Windows 未执行，原因是当前环境为 Linux；T056 的 Windows 覆盖将在后续最终复验中确认

## CTest 结果
- `ctest --test-dir build/linux --output-on-failure -R "NoKvSurfaceAudit"`：PASS
- 未运行 metadata / snapshot / recovery 测试集合；这些旧测试仍含 KV 符号依赖，按任务边界转交 `T057`

## KV residual status
- 生产代码中的 KV command / KV state machine / KV service / KV client / KV proto 主路径残留：已清理
- 生产构建图中的旧 KV source：已清理
- no-KV audit 当前仍有 tolerated blocker，主要是 tests 侧残留，需后续 `T057/T058` 收口

## 是否修改 tests/ 业务逻辑
- 否
- 本轮未迁移任何测试 case 语义

## 是否修改 proto / service / client / scripts
- proto：未修改，本轮确认 `proto/raft.proto` 已保持 metadata-only
- service：未恢复或新增任何 `KvService` 路径
- client：未修改 `raft_metadata_client`；`raft_kv_client` 仍保持已删除状态
- scripts：未修改 `test.sh` / `test.ps1`

## 是否进入 T057
- 可以进入 `T057`
- 前提说明：`T056` 已完成生产主路径清理；后续重点转为测试去重与旧 KV 断言迁移

## 剩余风险
- 旧测试仍直接依赖已删除的生产 KV 符号，后续重新构建测试目标时预期会失败
- `NoKvSurfaceAudit` 目前仍把部分 tests residual 视为 tolerated blocker，需 `T058` 升格为 strict fail
- `test.sh --group no-kv` 仍不是轻量入口，需 `T059` 修正
