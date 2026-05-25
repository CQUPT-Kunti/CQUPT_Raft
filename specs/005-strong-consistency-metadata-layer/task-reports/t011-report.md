# T011 任务报告

## T011 任务目标

根据 `T011 Implement request_id replay table for create and commit` 的要求，在 `modules/raft/state_machine/metadata_state_machine.cpp` 中实现 create / commit 的 request_id replay table，使相同 request_id 的重复请求能返回等价幂等结果，而不同 fingerprint / operation / object_key 的复用请求返回 idempotency conflict。

## 修改了哪些文件

- `modules/raft/state_machine/metadata_state_machine.cpp`
- `specs/005-strong-consistency-metadata-layer/task-reports/t011-report.md`

## 每个文件大概改了什么

### `modules/raft/state_machine/metadata_state_machine.cpp`

- 声明 `ComputeMetadataCommandFingerprint` 的最小原型，以复用 T005 的 fingerprint 实现。
- 在 `Apply` 中加入 request_id replay table 判定。
- 对同一 `request_id`：
  - 若 `operation`、`object_key`、`fingerprint` 一致，则返回幂等重放成功。
  - 若三者任一不一致，则返回 idempotency conflict。
- 在 create 成功后写入 replay entry，保存：
  - `request_id`
  - `operation`
  - `object_key`
  - `command_fingerprint`
  - `result_code`
  - `result_state`
  - `log_index`
  - `response_record`
- 在 commit 成功后同样写入 replay entry。
- 保持 T010 已有的 `NeverCreated -> Pending -> Committed` 状态转换与 committed-only visibility 不变。
- 未实现 delete replay、tombstone、snapshot save/load。

### `specs/005-strong-consistency-metadata-layer/task-reports/t011-report.md`

- 新增本次 T011 的独立任务报告。

## 是否执行了验证

- 已执行最小编译验证：
  - `c++ -std=c++20 -I modules -fsyntax-only modules/raft/state_machine/metadata_state_machine.cpp`
  - 结果：通过
- 未执行测试目录下测试。
  - 原因：本次任务不允许新增或修改测试文件，状态机测试由 T012 处理。

## 当前风险或后续事项

- 当前 replay table 只覆盖 create / commit，不覆盖 delete。
- 当前相同 request_id 的重复请求在 `Apply` 层返回幂等成功，但完整的外部结果码传播到 service / client 仍需后续任务接入。
- 当前 replay table 尚未进入 snapshot/restart 持久化边界；这部分由后续任务继续处理。
- 当前实现依赖 `.cpp` 内最小原型复用 codec/fingerprint，后续如果 common 头文件公开正式声明，可收敛接口暴露方式。

## 建议 commit message

```text
feat(state_machine): 实现 metadata create/commit 幂等重放表
```
