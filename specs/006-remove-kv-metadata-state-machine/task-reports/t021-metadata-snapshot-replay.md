# T021 MetadataStateMachine Snapshot + Replay 最小恢复验证

## 结论

- T021 已完成。
- 本次没有改 `MetadataStateMachine` 业务逻辑或 `RaftNode` wiring。
- 本次新增的是状态机级 `LoadSnapshot + 后续 MetadataCommand replay` 恢复验证。
- Linux 下已完成最小必要 configure / build / 定向 CTest 验证。

## 实际修改

- 更新 `tests/metadata_state_machine_test.cpp`
  - 新增 `SnapshotLoadThenReplayRestoresFinalStateAndBoundary`。
  - 测试流程：
    - 先 apply 一批 bucket/object 命令并保存 snapshot。
    - 新建 `MetadataStateMachine` 后 `LoadSnapshot`。
    - 先 replay 一条已包含在 snapshot 内的旧删除命令，验证只返回 `idempotent replay`，不会重复 apply，也不会回退 `last_applied_index`。
    - 再 replay 一组 `index > snapshot.last_applied_index` 的新命令，验证状态能继续推进到正确最终结果。
  - 覆盖的恢复事实：
    - `last_applied_index / last_applied_term` 边界正确推进。
    - `requests_ / request_fingerprints_` 恢复后仍保持幂等。
    - `tombstones_` 删除事实恢复后仍有效。
    - `objects_ / object_index_ / chunk_ref_index_` 在 replay 后保持一致。
    - snapshot 前已删除对象不会在 replay 后复活。
    - snapshot 前与 snapshot 后的 committed object 的 `ChunkRef` 都可查询。
    - `HeadObject / ListObjects` 与恢复后最终状态一致。
- 更新 `modules/raft/state_machine/AGENTS.md`
  - 补充 `MetadataStateMachine` 需要通过状态机级测试验证 snapshot 后 replay 边界和幂等延续。

## Linux 验证

- 选择原因
  - 本次只修改 `MetadataStateMachine` 相关测试和模块说明。
  - 按最小闭环执行 Linux configure + 受影响 target build + 对应测试过滤。
  - 不跑无关 target，不默认跑全量 CTest。

- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

- 结果
  - `cmake --preset debug-ninja-low-parallel`：PASS
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`：PASS
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`：PASS，`29/29` 通过

- 日志
  - configure 结果记录：`tmp/test-logs/t021-configure.log`
  - build 日志：`tmp/test-logs/t021-build.log`
  - ctest 日志：`tmp/test-logs/t021-ctest.log`

## 未跑全量 CTest 的说明

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 原因：本次只新增 `MetadataStateMachine` 的 snapshot+replay 恢复测试，不涉及默认 `RaftNode` 主路径、service、DataNode、restart recovery 接线或全局构建图。

## 风险与边界

- 当前验证的是状态机级 `snapshot + replay`，不是 `RaftNode` restart recovery。
- 本次 replay 测试只覆盖：
  - snapshot 内旧请求的重复回放不会重复 apply。
  - `index > last_applied_index` 的后续命令可继续推进状态。
- “真实 Raft log 扫描并按 `last_applied_index` 过滤回放”的接线仍留给后续 `RaftNode` 级任务。

## 验收结果

- snapshot + `MetadataCommand` replay 最小恢复验证：已完成
- `LoadSnapshot` 后继续 replay 后续 `MetadataCommand`：已验证
- replay 后 `last_applied_index / last_applied_term` 边界正确：已验证
- `request_table / request_fingerprints` 恢复后仍保持幂等：已验证
- `tombstone` 删除事实恢复后仍然有效：已验证
- `object_table / object_index / chunk_ref_index` 一致：已验证
- deleted object 不复活：已验证
- committed object 的 `ChunkRef` 可恢复并可查询：已验证
- `MetadataStateMachine` 不依赖 KV：保持成立
- 未修改 `RaftNode` 默认 wiring：保持成立
- 未删除 KV：保持成立
- 未实现 `RaftNode` restart recovery / service：保持成立
- 未进入 T022：保持成立

## 说明

- `tasks.md` 当前已有另一条不同含义的 `T021`，本次按用户明确指令执行并单独出具报告，未改 `tasks.md` 标记。
