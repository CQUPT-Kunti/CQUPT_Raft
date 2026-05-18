# T012 Report

## T012 任务目标

新增 `tests/metadata_state_machine_test.cpp`，覆盖 US1 的 metadata 状态机 MVP 行为：create 后 Pending 不可见、commit 后 Committed 可见、重复 create/commit 幂等、`request_id` 内容冲突和缺失 Pending 的 commit 错误。

## 修改了哪些文件

- `tests/metadata_state_machine_test.cpp`

## 每个文件大概改了什么

- `tests/metadata_state_machine_test.cpp`
  - 新增 `StrongConsistencyMetadataStateMachine` 单元测试。
  - 覆盖 create 后 `Head/List` 不可见。
  - 覆盖 commit 后 `Head/List` 可见。
  - 覆盖 duplicate create / duplicate commit 的幂等重放。
  - 覆盖同一 `request_id` 不同内容触发 `IDEMPOTENCY_CONFLICT`。
  - 覆盖 missing pending commit 返回明确错误。

## 是否执行了验证

- 已执行最小编译验证：
  - `c++ -std=c++20 -I modules -fsyntax-only tests/metadata_state_machine_test.cpp`
  - 原因：当前命令行环境未提供 `gtest/gtest.h` 头文件搜索路径。

## 当前风险或后续事项

- 当前只完成测试文件新增，未接入 CMake；测试 wiring 需由后续 T013 处理。
- 真实编译和运行仍依赖项目测试目标或 GoogleTest include/link 配置。

## 建议 commit message

```text
test(state_machine): 新增 metadata 状态机 MVP 单元测试
```
