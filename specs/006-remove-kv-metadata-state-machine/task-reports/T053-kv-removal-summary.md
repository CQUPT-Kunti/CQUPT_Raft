# T053 KV Removal Summary
## 修改内容
- 更新本汇总报告，按当前任务状态重写 KV removal、主路径切换、回归迁移与平台验证口径。
- 未修改业务源码。
## 使用的已有验证结果
- `t023-default-metadata-wiring.md`
- `t024-metadata-service-main-path.md`
- `t025-remove-kv-service-client.md`
- `T044-remove-kv-command-path.md`
- `T045-remove-kv-state-machine.md`
- `T046-retire-kv-service.md`
- `T047-retire-raft-kv-client.md`
- `T048-remove-kv-proto-surface.md`
- `T049-update-metadata-only-docs.md`
- `T050-no-kv-surface-audit.md`
- `T043-us4-recovery-validation.md`
- `T051-linux-final-validation.md`
- `T051-linux-failure-fix.md`
- `t027-windows-validation.md`
- 当前任务状态确认：Linux 与 Windows 最终验证已通过
## KV removal status
- `KvService`：已退役。
  - `kv_service_impl.h/.cpp` 已不存在。
  - `RaftNode::InitServer()` 不再注册 `KvService`。
- `raft_kv_client`：已退役。
  - `apps/raft_kv_client.cpp` 不存在。
  - `CMakeLists.txt` 不再包含 `raft_kv_client` target。
- `KvService / KvStatusCode / Put/Get/Delete proto`：已退役。
  - `proto/raft.proto` 不再包含 KV RPC/message。
  - `kv.proto` 已从主构建图移除。
- `CommandType::kSet / kDelete`：仍残留。
  - `T044` 明确判定为 blocked。
- `KvStateMachine / state_machine.h/.cpp / test_state_machine.cpp`：仍残留。
  - `T045` 明确判定为 blocked。
- `T044 / T045`：仍 blocked。
  - 结论：KV 物理删除未完成。
## metadata-only 主路径状态
- `MetadataService`：是当前唯一业务服务主路径。
  - `t024`、`t025`、`T046` 都确认写请求统一走 `MetadataCommand + ProposeMetadata(...)`。
- `MetadataStateMachine`：是当前默认业务状态机。
  - `t023` 明确确认默认 `RaftNode(config)` / `RaftNode(config, snapshot)` 已切到 `MetadataStateMachine`。
- `raft_metadata_client`：是当前唯一业务 CLI。
  - `T047` 确认 `raft_kv_client` 已退役。
- `no-KV surface audit`：已接入。
  - `T050` 已把 `NoKvSurfaceAudit` 接入 CMake / CTest / 脚本入口。
## 回归迁移完成度
- 已有覆盖：
  - metadata command / state machine / service / client scenario
  - bucket/object lifecycle
  - request_id 幂等、tombstone、object_index、chunk_ref 事实
  - leader switch / log replication / commit apply 的 metadata 迁移
  - snapshot / restart / replay / catch-up / recovery 的 metadata 迁移骨架
  - no-KV retired surface 审计
- 并发、幂等、leader switch：
  - 已有 `MetadataConcurrencyStressTest`、`MetadataFailoverTest`、leader switch 迁移测试
  - 当前任务状态下对应 Linux/Windows 最终验证已通过；历史失败记录保留在早期 task report 中
- snapshot / restart / replay / catch-up：
  - `T043`、`T051-linux-failure-fix.md` 记录了修复过程中的历史失败与收敛路径
  - 当前任务状态中，snapshot / restart / replay / catch-up 最终验证已通过
- 仍未完成或仍失败的旧 KV 回归迁移项：
  - `T044/T045` 对应的 KV command / KvStateMachine 删除链路仍未收口
  - `tests/support/raft_snapshot_restart_test_utils.h` 仍被 `T050` 审计列为 tolerated blocker
## 平台验证状态
- Linux final validation：通过。
  - 当前任务状态已确认 Linux 全量最终验证通过。
  - `T043`、`T051-linux-final-validation.md`、`T051-linux-failure-fix.md` 仍保留修复前/修复中的历史快照。
- Windows final validation：通过。
  - 当前任务状态已确认 Windows 最终验证通过。
  - `t027-windows-validation.md` 仍保留早期阶段性补测结果，不代表当前最终收口状态。
## 实际执行命令
- 本轮未跑业务回归，只做轻量验证：
  - `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client no_kv_surface_audit`
  - `ctest --test-dir build/linux --output-on-failure -R 'NoKvSurfaceAudit'`
- 结果：
  - build：PASS
  - `NoKvSurfaceAudit`：PASS
## 通过项
- `KvService` / `raft_kv_client` / KV proto 业务面已退出主路径
- 默认业务状态机与默认业务服务已切到 metadata-only
- `NoKvSurfaceAudit` 已接入并可通过
- Linux final validation 已通过
- Windows final validation 已通过
## 失败项
- 无新增平台验证失败项
- 历史失败记录仍存在于旧 task report，但当前任务状态已完成收口
## blocked 项
- `CommandType::kSet / kDelete`
- `KvStateMachine`
- `modules/raft/state_machine/state_machine.h/.cpp`
- `tests/test_state_machine.cpp`
- `T044`
- `T045`
## 结论
- 可以认为“主路径 metadata-only 已完成”：可以。
- 可以认为“KV 物理删除已完成”：不可以。
- 是否可以进入最终收尾：可以。
- 后续建议：
  - 先处理 `T044/T045` 的真实 blocker，再谈 KV 物理删除完成
  - 若需要补齐审计闭环，可在后续新增最终 Windows/Linux 收口报告，替代历史阶段性快照
