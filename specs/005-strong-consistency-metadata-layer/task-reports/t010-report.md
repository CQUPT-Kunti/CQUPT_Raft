# T010 任务报告

## T010 任务目标

根据 `T010 Implement create and commit state transitions` 的要求，在 `modules/raft/state_machine/metadata_state_machine.cpp` 中实现 `NeverCreated -> Pending -> Committed` 的最小状态转换闭环，并提供 committed-only 的 Head/List 查询行为。

## 修改了哪些文件

- `modules/raft/state_machine/metadata_state_machine.cpp`
- `specs/005-strong-consistency-metadata-layer/task-reports/t010-report.md`

## 每个文件大概改了什么

### `modules/raft/state_machine/metadata_state_machine.cpp`

- 新增 `StrongConsistencyMetadataStateMachine` 的 `.cpp` 实现。
- 实现 `Apply` 对 metadata create / commit command 的最小分发。
- create 路径：
  - 只创建 `Pending` 记录
  - 初始化 `created_at_log_index`
  - 清理 commit/delete 相关字段
- commit 路径：
  - 只允许已有 `Pending` 记录进入 `Committed`
  - 设置 `commit_request_id`、`committed_at_log_index`、`commit_info`
- 实现 committed-only 查询：
  - `HeadMetadataRecord` 仅返回 `Committed`
  - `ListMetadataRecords` 仅返回 `Committed`
  - `Pending` 对 Head/List 不可见
- `SaveSnapshot` / `LoadSnapshot` 当前仅返回未实现错误，占位到 T020。
- 未实现 request_id replay table 逻辑。
- 未实现 delete / tombstone 逻辑。

### `specs/005-strong-consistency-metadata-layer/task-reports/t010-report.md`

- 新增本次 T010 的独立任务报告。

## 是否执行了验证

- 已执行最小编译验证：
  - `c++ -std=c++20 -I modules -fsyntax-only modules/raft/state_machine/metadata_state_machine.cpp`
  - 结果：通过
- 未执行测试目录下测试。
  - 原因：本次任务只实现最小状态机 `.cpp`，未进入状态机测试任务。

## 当前风险或后续事项

- 当前 `Apply` 只覆盖 create / commit 的最小闭环，没有实现 T011 的 request_id replay table。
- 当前 `SaveSnapshot` / `LoadSnapshot` 只是未实现占位，真正的 metadata snapshot/restart 逻辑仍由 T020 完成。
- 当前 delete / tombstone 尚未实现，相关生命周期由 T018 继续处理。
- 当前通过在 `.cpp` 内声明最小 codec 原型复用 T005 产物；后续如果 common 头文件公开正式声明，可再收敛接口暴露方式。

## 建议 commit message

```text
feat(state_machine): 实现 metadata create/commit 最小状态转换
```
