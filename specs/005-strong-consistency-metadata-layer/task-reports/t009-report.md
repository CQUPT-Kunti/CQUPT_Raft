# T009 任务报告

## T009 任务目标

根据 `T009 Add state machine API declarations` 的要求，在 `modules/raft/state_machine/metadata_state_machine.h` 中声明 `StrongConsistencyMetadataStateMachine` 的公共接口边界，确保其继承并兼容现有 `IStateMachine`，同时为 committed-only 查询、snapshot/restart 和 request_id 幂等表保留后续实现入口。

## 修改了哪些文件

- `modules/raft/state_machine/metadata_state_machine.h`
- `specs/005-strong-consistency-metadata-layer/task-reports/t009-report.md`

## 每个文件大概改了什么

### `modules/raft/state_machine/metadata_state_machine.h`

- 新增 `MetadataHeadRequest` / `MetadataHeadResponse`。
- 新增 `MetadataListRequest` / `MetadataListResponse`。
- 新增 `StrongConsistencyMetadataStateMachine` 类声明。
- 该类继承 `IStateMachine`，并声明：
  - `Apply`
  - `SaveSnapshot`
  - `LoadSnapshot`
  - `HeadMetadataRecord`
  - `ListMetadataRecords`
- 在 private 区声明后续实现所需的边界成员：
  - `records_`
  - `tombstones_`
  - `replay_table_`
  - `mu_`
- 没有实现 create/commit/delete 状态转换，也没有实现幂等表逻辑。

### `specs/005-strong-consistency-metadata-layer/task-reports/t009-report.md`

- 新增本次 T009 的独立任务报告。

## 是否执行了验证

- 已执行最小头文件语法验证：
  - `c++ -std=c++20 -I modules -x c++-header -fsyntax-only modules/raft/state_machine/metadata_state_machine.h`
  - 结果：通过
  - 附带一个 `#pragma once in main file` 告警，这是头文件直接作为编译入口时的常见告警，不是错误
- 未执行测试目录下测试。
  - 原因：本次任务只做接口声明，不涉及测试接入或运行。

## 当前风险或后续事项

- 当前只完成接口边界声明，没有进入 T010 的 `Pending -> Committed` 状态转换实现。
- `HeadMetadataRecord` / `ListMetadataRecords` 目前只表达 committed-only 查询边界，具体过滤、排序和分页行为仍需后续 `.cpp` 实现。
- `replay_table_` 仅保留幂等表边界，具体 request_id replay 逻辑仍由 T011 处理。

## 建议 commit message

```text
feat(state_machine): 新增 metadata state machine 接口声明
```
