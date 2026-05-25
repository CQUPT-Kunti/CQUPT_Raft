# T006 任务报告

## T006 任务目标

根据 `T006 Add metadata status mapping contract` 的要求，在 `common` 层定义 metadata API 的结果码和响应摘要类型，供后续状态机、service 和 client 共享结果语义，同时避免引入 gRPC/protobuf 依赖。

## 修改了哪些文件

- `modules/raft/common/metadata_result.h`
- `specs/005-strong-consistency-metadata-layer/task-reports/t006-report.md`

## 每个文件大概改了什么

### `modules/raft/common/metadata_result.h`

- 新增 `MetadataStatusCode`，覆盖：
  - `OK`
  - `NOT_LEADER`
  - `INVALID_ARGUMENT`
  - `NOT_FOUND`
  - `IDEMPOTENT_REPLAY`
  - `IDEMPOTENCY_CONFLICT`
  - `STATE_CONFLICT`
  - `INTERNAL_ERROR`
  - `TIMEOUT`
- 新增 `MetadataLeaderHint`，表达 `leader_id` 和 `leader_address`。
- 新增 `MetadataResponseSummary`，表达：
  - `request_id`
  - `object_key`
  - `result_state`
  - `term`
  - `log_index`
  - `leader_hint`
  - `message`
- 新增 `MetadataResult`，作为通用结果对象，封装状态码和响应摘要。
- 新增少量 inline helper，保持头文件轻量，不引入业务流程、文件 IO 或 RPC 依赖。

### `specs/005-strong-consistency-metadata-layer/task-reports/t006-report.md`

- 新增本次 T006 的独立任务报告。

## 是否执行了验证

- 已执行最小编译验证：
  - `c++ -std=c++20 -x c++-header -fsyntax-only modules/raft/common/metadata_result.h`
  - 结果：通过
  - 附带一个 `#pragma once in main file` 告警，这是头文件直接作为编译入口时的常见告警，不是错误
- 未执行测试目录下测试。
  - 原因：本次允许读取范围不包含 `tests/**`，且 T007 尚未开始。

## 当前风险或后续事项

- 当前只完成 common 层结果语义定义，尚未在状态机、service、client 中接入。
- `result_state` 使用对 `MetadataRecordState` 的前置声明，以避免在本任务中扩大读取和依赖范围；后续接入时需确保与领域模型保持一致。
- 本次没有进入 T007 或后续任务。

## 建议 commit message

```text
feat(common): 新增 metadata 结果码与响应摘要类型
```
