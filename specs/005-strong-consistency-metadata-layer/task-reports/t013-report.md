# T013 Report

## T013 任务目标

将 `tests/metadata_state_machine_test.cpp` 接入 CMake/CTest，正式编译并运行 `MetadataStateMachineTest`，只做测试 wiring 和验证，不改状态机业务逻辑。

## 修改了哪些文件

- `tests/CMakeLists.txt`

## 每个文件大概改了什么

- `tests/CMakeLists.txt`
  - 新增 `test_metadata_state_machine` 测试 target。
  - 编入 `metadata_state_machine_test.cpp`、`metadata_state_machine.cpp`、`metadata_command.cpp`。
  - 链接 `raft_core` 和 `GTest::gtest_main`。
  - 使用 `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST ...)` 接入 CTest。

## 是否执行了验证

- 已执行：
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
  - `ctest --test-dir build/linux --output-on-failure -R '^MetadataStateMachineTest\.'`
- 结果：6/6 通过。

## 当前风险或后续事项

- 本次只完成 T013 的测试 wiring 和验证。
- 未进入 T014 的 proto / service 契约任务。

## 建议 commit message

```text
test(cmake): 接入 metadata state machine 单元测试构建
```
