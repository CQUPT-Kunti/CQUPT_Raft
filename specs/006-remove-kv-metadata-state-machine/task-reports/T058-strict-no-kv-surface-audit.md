# T058 Strict No-KV Surface Audit

## 本次修改文件
- `tests/no_kv_surface_audit.cmake`
- `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`

## strict audit 新规则
- `NoKvSurfaceAudit` 不再保留 `T050` 风格的 tolerated blocker 列表
- 生产代码 strict fail：
  - 旧 KV 文件路径：`kv_service_impl.*`、`apps/raft_kv_client.cpp`、`proto/kv.proto`、`modules/raft/state_machine/state_machine.h/.cpp`
  - 旧 KV build/test target：`raft_kv_client`、`kv_service_impl`、`test_kv_service`、`test_state_machine`
  - 旧 KV 生产符号：`CommandType::kSet`、`CommandType::kDelete`、裸 `kSet/kDelete`、`KvStateMachine`、`CompositeKvMetadataStateMachine`、`KvService`、`KvStatusCode`、`raft_kv_client`、`Put/Get/DeleteRequest`、`SET|`、`DEL|`、`DebugGetValue`、`kv-service`、`kv.proto`
  - 旧 KV include/source 路径：`raft/state_machine/state_machine.h`、`modules/raft/state_machine/state_machine.cpp`
- tests 主路径 strict fail：
  - `tests/test_state_machine.cpp`、`tests/test_kv_service.cpp`
  - `CommandType::kSet`、`CommandType::kDelete`
  - `SetCommand(`、`DeleteCommand(`
  - `DebugGetValue`
  - `raft/state_machine/state_machine.h`
  - `KvStateMachine`
  - `KvStateMachineTest`
  - `test_state_machine`
  - `KV regression-only path`

## 允许保留路径说明
- `specs/006-remove-kv-metadata-state-machine/task-reports/**`
- `specs/006-remove-kv-metadata-state-machine/research.md`
- `specs/006-remove-kv-metadata-state-machine/plan.md`
- `specs/006-remove-kv-metadata-state-machine/spec.md`
- `specs/006-remove-kv-metadata-state-machine/tasks.md`
- `specs/006-remove-kv-metadata-state-machine/quickstart.md`
- `specs/006-remove-kv-metadata-state-machine/task-reports/cross-task-risk-notes.md`
- `tests/no_kv_surface_audit.cmake`
- `tests/AGENTS.md`、`tests/support/AGENTS.md`
- `tests/test-reports/**`

## 删除 / 取消的 tolerated blocker
- 删除 `T050` 中对以下 blocker 的 tolerated 处理：
  - `modules/raft/common/command.h/.cpp` 中的 `CommandType::kSet/kDelete`
  - `modules/raft/state_machine/state_machine.h/.cpp`
  - `tests/test_state_machine.cpp`
  - `tests/support/raft_snapshot_restart_test_utils.h` 中旧 `SetCommand/DeleteCommand`
- 这些路径如果重新出现，现在会直接触发 strict fail

## deferred 风险
- `test.ps1` 仍保留 `KvStateMachineTest` fallback 子集说明
- `CMakePresets.json` 仍保留 `KvStateMachineTest` fallback filter
- 本轮不修改它们，作为 `T059` 转交项，只在 audit 输出中以 deferred risk 方式提示，不纳入 T058 strict-fail 范围

## NoKvSurfaceAudit 执行结果
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`：PASS
- build 输出已打印：
  - whitelist scope
  - deferred risks
  - strict metadata-only pass 结论

## Linux 结果
- `ctest --test-dir build/linux --output-on-failure -R "NoKvSurfaceAudit"`：PASS
- `1/1 PASS`

## Windows 结果
- Windows 未执行，原因是当前环境为 Linux；T058 的 Windows 覆盖将在后续最终复验中确认

## CTest 结果
- 本轮只运行 `NoKvSurfaceAudit`
- 未运行全量 CTest
- 未修改 recovery 测试并行策略

## KV residual status
- 生产代码主路径：strict no-KV audit 已覆盖
- tests 主测试路径：strict no-KV audit 已覆盖
- 历史 task report / migration docs：已通过白名单隔离，不会误杀
- audit 自身检测关键词：已通过白名单隔离，不会误杀
- 剩余 residual 主要在脚本 / preset fallback 入口，已转交 `T059`

## 是否修改生产代码
- 否

## 是否修改业务测试逻辑
- 否

## 是否进入 T059
- 可以进入 `T059`

## 剩余风险
- `test.ps1` 与 `CMakePresets.json` 的 `KvStateMachineTest` fallback 文案 / filter 仍未收口
- `test.sh --group no-kv` 仍不是轻量入口，需 `T059` 处理
- recovery 测试并行互扰风险仍在，后续脚本分组应继续保持低并发或显式串行
