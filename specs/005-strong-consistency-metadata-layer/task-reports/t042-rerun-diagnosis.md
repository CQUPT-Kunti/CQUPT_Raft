# T042-rerun Diagnosis

## 执行命令列表

1. `ctest --test-dir build/windows -C Debug -N -R "Metadata"`
2. `ctest --test-dir build/windows --output-on-failure -C Debug -R "Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test"`
3. `cmake --build --preset windows-debug --target test_metadata_command test_metadata_manifest test_metadata_state_machine test_metadata_snapshot test_metadata_failover test_metadata_client_scenario`
4. `ctest --test-dir build/windows -C Debug -N -R "Metadata"`
5. `ctest --test-dir build/windows --output-on-failure -C Debug -R "Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test"`

## 每个命令 PASS / FAIL

1. `ctest-list-metadata`: `FAIL`
   - exit code: `0`
   - 结果摘要: 只列出了 2 个名称中包含 `Metadata` 的非目标测试，未列出 metadata suite。

2. `ctest-run-metadata`: `FAIL`
   - exit code: `0`
   - 关键输出: `No tests were found!!!`

3. `build-metadata-tests`: `PASS`
   - exit code: `0`
   - 结果摘要: 6 个 metadata test target 全部成功构建：
     - `test_metadata_command.exe`
     - `test_metadata_manifest.exe`
     - `test_metadata_state_machine.exe`
     - `test_metadata_snapshot.exe`
     - `test_metadata_failover.exe`
     - `test_metadata_client_scenario.exe`

4. `ctest-list-after-build`: `PASS`
   - exit code: `0`
   - 结果摘要:
     - 构建后已发现 34 个目标 metadata tests
     - 由于 `-R "Metadata"` 也匹配到 2 个其他测试，列表总数为 `36`

5. `ctest-run-after-build`: `FAIL`
   - exit code: `8`
   - 结果摘要: metadata suite 已实际运行，`34` 个目标测试中 `29` 个通过，`5` 个失败

## 错误分类

- 最终错误分类: `测试断言失败`
- 前置诊断结论:
  - 初始失败阶段属于 `测试 target 未构建或 CTest 占位项未替换`
  - 显式构建 metadata test targets 后，该问题已消失
  - 当前真正阻塞 `T042` 的错误已经收敛为 Windows 下 `MetadataClientScenarioTest` 的断言失败

## 关键错误摘要

### 初始阶段

- 初次运行 metadata suite 时输出:

```text
Test project D:/Code/C++/CQUPT_Raft/build/windows
No tests were found!!!
```

- 说明当时 metadata tests 尚未被发现为可运行测试。

### 构建后阶段

- 构建后 `ctest -N -R "Metadata"` 已能列出 `MetadataCommandTest`、`MetadataManifestTest`、`MetadataStateMachineTest`、`MetadataSnapshotTest`、`MetadataFailoverTest`、`MetadataClientScenarioTest` 对应测试。
- 说明 `CTest 注册/发现失败` 不是最终根因，metadata tests 本身在显式构建后可以被发现和运行。

### 最终失败测试

失败测试共 `5` 个，全部来自 `MetadataClientScenarioTest`：

1. `MetadataClientScenarioTest.CreateScenarioBuildsMetadataOnlyManifest`
2. `MetadataClientScenarioTest.CreateCommitHeadListDeleteFlowSucceeds`
3. `MetadataClientScenarioTest.VerifyReadAfterWriteModeReportsPass`
4. `MetadataClientScenarioTest.DuplicateRequestIdDoesNotCreateDuplicateVisibleRecord`
5. `MetadataClientScenarioTest.PayloadBoundaryAndMockLocationsBehaviorAreExposed`

共同关键错误：

```text
The filename, directory name, or volume label syntax is incorrect.
```

共同断言形态：

```text
error: Expected equality of these values:
  result.exit_code
    Which is: 1
  0
```

对应测试文件断言位置：

- `tests/metadata_client_scenario_test.cpp(640)`
- `tests/metadata_client_scenario_test.cpp(668)`
- `tests/metadata_client_scenario_test.cpp(748)`
- `tests/metadata_client_scenario_test.cpp(772)`
- `tests/metadata_client_scenario_test.cpp(830)`

整体结果摘要：

```text
85% tests passed, 5 tests failed out of 34
```

## 日志文件路径

- 日志目录:
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-rerun-logs/`

- 具体日志:
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-rerun-logs/ctest-list-metadata.log`
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-rerun-logs/ctest-run-metadata.log`
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-rerun-logs/build-metadata-tests.log`
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-rerun-logs/ctest-list-after-build.log`
  - `D:/Code/C++/CQUPT_Raft/specs/005-strong-consistency-metadata-layer/task-reports/t042-rerun-logs/ctest-run-after-build.log`

## 是否需要 T042-fix

- `需要`
- 理由:
  - metadata test targets 已可成功构建
  - metadata suite 已可被 `CTest` 发现并运行
  - 当前失败已经明确收敛为 Windows 下 `MetadataClientScenarioTest` 的 5 个断言失败
  - 从错误文本看，存在明显的 Windows 文件名/目录语法错误；这更像是待修复的 Windows 路径/命令调用兼容性问题，而不是单纯的测试未注册问题

## 边界确认

- 未修改源码
- 未修改测试
- 未修改 CMake
- 未修改 `tasks.md`
- 未进入 `T043`
