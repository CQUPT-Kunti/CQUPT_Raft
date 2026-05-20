# T042 Windows CTest Validation Report

## Task Scope

- Task ID: `T042`
- Goal: 在 Windows 平台执行 metadata 相关 unit / integration `CTest` 验证。
- In Scope:
  - `MetadataCommandTest`
  - `MetadataManifestTest`
  - `MetadataStateMachineTest`
  - `MetadataSnapshotTest`
  - `MetadataFailoverTest`
  - `MetadataClientScenarioTest`
- Out of Scope:
  - 不执行 `T043`
  - 不执行 Metadata Client runtime flow
  - 不修改源码、测试、CMake、`tasks.md` 或高频文档

## Environment

- OS: `Microsoft Windows 11 家庭版 中文版`
- OS version: `Microsoft Windows NT 10.0.26200.0`
- Shell: `PowerShell Core`
- CMake/CTest: `4.1.0-rc1`
- Build directory: `D:/Code/C++/CQUPT_Raft/build/windows`
- Generator: `Visual Studio 17 2022`
- Compiler: `MSVC cl.exe 14.42.34433`
- Configuration: `Debug`

## Validation Basis

- Validation matrix target mapping confirms the Windows metadata suite should cover:
  - `^MetadataCommandTest\.`
  - `^MetadataManifestTest\.`
  - `^MetadataStateMachineTest\.`
  - `^MetadataSnapshotTest\.`
  - `^MetadataFailoverTest\.`
  - `^MetadataClientScenarioTest\.`
- Expected combined result count from current matrix: `34` tests

## Commands Executed

1. Primary validation command

```powershell
ctest --test-dir build/windows --output-on-failure -C Debug -R "Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test"
```

2. Minimal diagnostic command after failure

```powershell
ctest --test-dir build/windows -N -C Debug
```

## Result

- T042 result: `FAIL`
- Failure stage: `CTest selection / execution preparation`
- Failure classification: `metadata test targets not built in current Windows build directory`

## Failure Evidence

### Primary command result

- Exit code: `0`
- Elapsed: `0.237s`
- Key output:

```text
Test project D:/Code/C++/CQUPT_Raft/build/windows
No tests were found!!!
```

### Diagnostic result

- `ctest -N -C Debug` shows the build directory does contain metadata-related test registrations, but they are registered as `_NOT_BUILT` placeholders rather than runnable test executables.
- Relevant registered entries:

```text
Could not find executable test_metadata_command_NOT_BUILT
  Test   #6: test_metadata_command_NOT_BUILT
Could not find executable test_metadata_manifest_NOT_BUILT
  Test   #7: test_metadata_manifest_NOT_BUILT
Could not find executable test_metadata_state_machine_NOT_BUILT
  Test   #8: test_metadata_state_machine_NOT_BUILT
Could not find executable test_metadata_snapshot_NOT_BUILT
  Test   #9: test_metadata_snapshot_NOT_BUILT
Could not find executable test_metadata_failover_NOT_BUILT
  Test  #10: test_metadata_failover_NOT_BUILT
Could not find executable test_metadata_client_scenario_NOT_BUILT
  Test  #11: test_metadata_client_scenario_NOT_BUILT
```

### Last 50 lines of relevant failure output

```text
Test project D:/Code/C++/CQUPT_Raft/build/windows
  Test   #1: CommandTest.SetCommandSerializeAndDeserialize
  Test   #2: CommandTest.DeleteCommandSerializeAndDeserialize
  Test   #3: CommandTest.EmptyKeyIsInvalid
  Test   #4: CommandTest.UnknownCommandIsInvalid
  Test   #5: CommandTest.DeserializeRejectsBadInput
Could not find executable test_metadata_command_NOT_BUILT
Looked in the following places:
test_metadata_command_NOT_BUILT
test_metadata_command_NOT_BUILT.exe
Debug/test_metadata_command_NOT_BUILT
Debug/test_metadata_command_NOT_BUILT.exe
Debug/test_metadata_command_NOT_BUILT
Debug/test_metadata_command_NOT_BUILT.exe
  Test   #6: test_metadata_command_NOT_BUILT
Could not find executable test_metadata_manifest_NOT_BUILT
Looked in the following places:
test_metadata_manifest_NOT_BUILT
test_metadata_manifest_NOT_BUILT.exe
Debug/test_metadata_manifest_NOT_BUILT
Debug/test_metadata_manifest_NOT_BUILT.exe
Debug/test_metadata_manifest_NOT_BUILT
Debug/test_metadata_manifest_NOT_BUILT.exe
  Test   #7: test_metadata_manifest_NOT_BUILT
Could not find executable test_metadata_state_machine_NOT_BUILT
Looked in the following places:
test_metadata_state_machine_NOT_BUILT
test_metadata_state_machine_NOT_BUILT.exe
Debug/test_metadata_state_machine_NOT_BUILT
Debug/test_metadata_state_machine_NOT_BUILT.exe
Debug/test_metadata_state_machine_NOT_BUILT
Debug/test_metadata_state_machine_NOT_BUILT.exe
  Test   #8: test_metadata_state_machine_NOT_BUILT
Could not find executable test_metadata_snapshot_NOT_BUILT
Looked in the following places:
test_metadata_snapshot_NOT_BUILT
test_metadata_snapshot_NOT_BUILT.exe
Debug/test_metadata_snapshot_NOT_BUILT
Debug/test_metadata_snapshot_NOT_BUILT.exe
Debug/test_metadata_snapshot_NOT_BUILT
Debug/test_metadata_snapshot_NOT_BUILT.exe
  Test   #9: test_metadata_snapshot_NOT_BUILT
Could not find executable test_metadata_failover_NOT_BUILT
Looked in the following places:
test_metadata_failover_NOT_BUILT
test_metadata_failover_NOT_BUILT.exe
Debug/test_metadata_failover_NOT_BUILT
Debug/test_metadata_failover_NOT_BUILT.exe
Debug/test_metadata_failover_NOT_BUILT
Debug/test_metadata_failover_NOT_BUILT.exe
  Test  #10: test_metadata_failover_NOT_BUILT
Could not find executable test_metadata_client_scenario_NOT_BUILT
...
```

## Assessment

- 当前 Windows `CTest` 环境可用，因此本任务不是 `BLOCKED`。
- 但当前 `build/windows` 中 metadata 相关测试未处于可执行状态，导致：
  - 目标 regex `Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test` 无法选中任何可运行测试
  - metadata suite 无法形成 `34/34 PASS`
- 因此本次 `T042` 验收标准未满足。

## Final Root Cause Note

- 本次 `T042` 首次红灯时，表面现象是：
  - Windows `build/windows` 中 metadata tests 仍是 `*_NOT_BUILT` 占位项；
  - `ctest` 直接返回 `No tests were found!!!`
- 之后通过 `T042-rerun` 继续诊断并显式构建 metadata test targets 后，确认这不是最终根因，只是第一次失败时的直接表现。
- 真正导致 `T042` 红灯持续存在的底层错误是：
  - Windows 下 `MetadataClientScenarioTest` 的测试 harness 通过外部 shell 命令启动 `raft_metadata_client`；
  - 命令拼接、路径引号、重定向或工作目录处理与 Windows 不兼容；
  - 最终触发错误：

```text
The filename, directory name, or volume label syntax is incorrect.
```

- 该错误进一步表现为：
  - `MetadataClientScenarioTest` 的 5 个用例返回 `result.exit_code == 1`
  - 因而 metadata suite 无法达到 `34/34 PASS`
- 结论：
  - `T042` 首次失败的直接原因是测试 target 未构建；
  - `T042` 真正的持续红灯根因是 Windows 下 `MetadataClientScenarioTest` 的命令执行/路径处理兼容性错误，而不是 metadata 业务逻辑错误。

## Boundary Confirmation

- 未修改源码。
- 未修改测试。
- 未修改 `CMakeLists.txt` / `CMakePresets.json`。
- 未修改 `tasks.md`。
- 未修改 `validation-matrix.md`。
- 未执行 `T043`。
- 未进入 Linux 验证任务。
- 未读取 `specs/004-raft-industrialization/**`。

## Log Retention Note

- 按本任务写入约束，本次只创建/更新本报告文件，未额外新建独立 `.log` 文件。
- 本次完整关键执行记录以本报告为准：
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-report.md`

## Conclusion

- T042 acceptance result: `FAIL`
- Next step: `不自动进入 T043`
