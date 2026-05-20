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
