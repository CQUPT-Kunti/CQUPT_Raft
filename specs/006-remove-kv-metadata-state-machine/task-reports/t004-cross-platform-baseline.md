# T004 跨平台验证基线

## 1. T004 结论

- 结论分类：`Linux=通过`，`Windows=未执行（当前环境不支持）`。
- 当前代码状态下，Linux 既有工作流可完成 `configure/build/CTest` 全流程。
- 当前 Linux 全量基线通过，但通过路径仍显著依赖 `KVStateMachine` / `KvService` / `CompositeKvMetadataStateMachine`。
- 当前 Windows 基线未写成通过；原因是本机为 Linux shell 环境，`windows` preset 依赖 `Visual Studio 17 2022`，无法进入正式 configure/build/CTest 工作流。
- 本报告用于后续 006 删除 KV 后做前后对照，不包含任何修复动作。

## 2. Linux 验证结果

- 环境：当前会话为 Linux `bash`，工作目录 `/home/yangjilei/Code/C++/CQUPT_Raft`。
- 入口工作流：`cmake --preset debug-ninja-low-parallel`、`cmake --build --preset debug-ninja-low-parallel`、`CTEST_PARALLEL_LEVEL=1 ./test.sh --group all`。
- `configure` 第一次在沙箱内失败，失败原因为 `vcpkg` 文件锁无法获取，属于执行环境限制，不属于仓库代码失败。
- `configure` 在非沙箱环境重跑后通过。
- Linux configure 命令：`cmake --preset debug-ninja-low-parallel`
- Linux configure 结果：`PASS`
- Linux configure 耗时：约 `2s`
- Linux configure 日志：[tmp/test-logs/t004-linux-configure.log](/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t004-linux-configure.log)
- Linux build 命令：`cmake --build --preset debug-ninja-low-parallel`
- Linux build 结果：`PASS`
- Linux build 摘要：`ninja: no work to do.`
- Linux build 日志：[tmp/test-logs/t004-linux-build.log](/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t004-linux-build.log)
- Linux CTest 命令：`CTEST_PARALLEL_LEVEL=1 ./test.sh --group all`
- Linux CTest 结果：`PASS`
- Linux CTest 统计：`138/138` 通过，`0` 失败。
- Linux CTest 总耗时：日志尾部记录 `444.21 sec`；包装命令墙钟约 `867s`。
- Linux CTest 日志：[tmp/test-logs/t004-linux-ctest.log](/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t004-linux-ctest.log)
- Linux 已确认覆盖的关键测试面：leader election、log replication、commit/apply、split brain、persistence、snapshot recovery、snapshot restart、follower catch-up、segment storage、snapshot storage、replicator behavior。
- Linux 当前未观察到测试失败或跳过项。

## 3. Windows 验证结果

- 目标工作流来源：`CMakePresets.json` 中的 `windows` / `windows-debug` / `windows-release` configure preset，`windows-debug-tests` / `windows-release-tests` test preset，以及 `test.ps1`。
- 当前环境未具备真实 Windows toolchain / generator。
- 为确认原因，执行了 `cmake --preset windows`。
- Windows configure 结果：`未执行成功`
- Windows configure 摘要：`Could not create named generator Visual Studio 17 2022`
- Windows configure 日志：[tmp/test-logs/t004-windows-configure-attempt.log](/home/yangjilei/Code/C++/CQUPT_Raft/tmp/test-logs/t004-windows-configure-attempt.log)
- `build/windows/CMakeCache.txt` 当前不存在，说明未形成可继续复用的 Windows build tree。
- Windows build 结果：`未执行`
- Windows CTest 结果：`未执行`
- 未执行原因：当前会话为 Linux 环境，缺少 `Visual Studio 17 2022` generator，无法进入 `test.ps1` 的正式 Windows configure/build/CTest 基线。
- 结论：Windows 只能记录为“环境未满足，未执行”，不能写成通过。

## 4. 当前失败 / 跳过项

- Linux：无失败项。
- Linux：无显式跳过项。
- Windows：非代码失败，属于环境不可执行；`configure` 在 generator 阶段终止，`build/CTest` 未开始。
- 当前可确认的既有基线问题：Linux 沙箱内直接 configure 会被 `vcpkg` 文件锁阻塞，但非沙箱重跑后通过；这属于执行环境问题，不归因到 006 删除 KV。

## 5. 与 006 删除 KV 相关的风险

- `raft_core` 仍编译 `modules/raft/service/kv_service_impl.cpp` 与 `modules/raft/state_machine/state_machine.cpp`。
- `raft_kv_client` 仍是主构建 target。
- `test.sh` 仍包含 `kv-service` 分组，且 `unit` 分组显式包含 `KvStateMachineTest`。
- `test.ps1` 的 fallback subset 仍写死 `CommandTest / KvStateMachineTest / TimerSchedulerTest / ThreadPoolTest`。
- 默认 `RaftNode` 仍装配 `CompositeKvMetadataStateMachine`，并非 metadata-only 路径。
- 当前恢复类回归虽然通过，但大量断言仍基于 `DebugGetValue()`、`SET/DEL`、KV 可见性，而不是 `request_table / tombstone / object_index / last_applied_index / last_applied_term` 的完整元数据事实。
- 因此，删除 KV 后最先受影响的不会只是 demo，而是主回归路径、恢复路径和跨平台测试入口。

## 6. 当前测试仍依赖 KV 的基线事实

- `RaftKvServiceTest.*` 直接验证 `KvService` 的 Put/Get/Delete/redirect/status/health。
- `KvStateMachineTest.*` 与 `CommandTest.*` 直接验证 KV 命令和 KV 状态机。
- 多个 Raft 回归测试通过 `DebugGetValue()` 断言 `SET/DEL` 最终状态，包括 persistence、snapshot、catch-up、integration、replicator、leader switch、segment storage 相关测试。
- `modules/raft/node/raft_node.cpp` 默认构造与 `InitServer()` 仍绑定 `CompositeKvMetadataStateMachine` 和 `KvServiceImpl`。
- 这意味着当前 Linux 138/138 通过的基线，本质上仍是“Raft + KV/Composite”基线，不是 metadata-only 基线。

## 7. 后续对照建议

- 删除 KV 后需要对照复跑 Linux `configure/build/CTest` 全流程，目标至少保持与当前 Linux 基线同等级覆盖。
- Windows 后续必须在真实 Windows 环境补齐 `configure/build/CTest` 三段结果，不能继续用 Linux 侧替代。
- 恢复类对照不能只看对象可见性；后续必须显式补 `request_table`、`tombstone`、`object_table/object_index`、`snapshot.meta` applied boundary 一致性断言。
- follower catch-up、restart recovery、snapshot replay、leader switch after concurrent writes 应保持 Linux + Windows 双平台保留。
- 后续需要同步更新的 `AGENTS.md` 至少包括：根 `AGENTS.md`、[apps/AGENTS.md](/home/yangjilei/Code/C++/CQUPT_Raft/apps/AGENTS.md)、[modules/raft/service/AGENTS.md](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/service/AGENTS.md)、[modules/raft/state_machine/AGENTS.md](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/state_machine/AGENTS.md)、[modules/raft/node/AGENTS.md](/home/yangjilei/Code/C++/CQUPT_Raft/modules/raft/node/AGENTS.md)、[proto/AGENTS.md](/home/yangjilei/Code/C++/CQUPT_Raft/proto/AGENTS.md)。

## 8. 验收结果

- 已生成 [t004-cross-platform-baseline.md](/home/yangjilei/Code/C++/CQUPT_Raft/specs/006-remove-kv-metadata-state-machine/task-reports/t004-cross-platform-baseline.md)。
- 已记录 Linux `configure/build/CTest` 结果。
- 已记录 Windows `configure/build/CTest` 结果，其中 build/CTest 明确标记为“未执行”并说明原因。
- 已记录当前失败 / 跳过项与平台差异。
- 已记录与 006 删除 KV 直接相关的基线风险。
- 已形成后续 metadata-only 重构后的对照基线。
- 本次未修改源码、测试、CMake、proto、`AGENTS.md`、`spec.md`、`plan.md`、`tasks.md`。
- 本次未进入 T005 或后续任务。
