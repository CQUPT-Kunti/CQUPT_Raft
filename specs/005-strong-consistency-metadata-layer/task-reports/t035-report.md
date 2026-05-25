# T035 执行报告

## 任务范围

- 任务编号：`T035`
- 任务目标：回填 `specs/005-strong-consistency-metadata-layer/api.md` 中已经固定的 Metadata API 语义。
- 本次仅处理：
  - `specs/005-strong-consistency-metadata-layer/api.md`
- 本次未执行：
  - `T036` 及后续任务
  - 任意源码修改
  - 任意测试修改
  - 任意构建或测试命令

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守其中禁止路径。
- 按任务与用户约束，重点读取了：
  - `specs/005-strong-consistency-metadata-layer/tasks.md`
  - `specs/005-strong-consistency-metadata-layer/api.md`
- 为对齐当前已固定契约和既有实现报告，最小补充读取了：
  - `proto/raft.proto`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t014-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t015-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t018-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t019-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t022-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t024-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t034-report.md`

## 文档回填内容

- 将 `api.md` 从“规划/开放语句”收口为“当前固定语义”。
- 明确 `MetadataStatusCode` 各状态码的固定使用场景：
  - `OK`
  - `NOT_LEADER`
  - `INVALID_ARGUMENT`
  - `NOT_FOUND`
  - `IDEMPOTENT_REPLAY`
  - `IDEMPOTENCY_CONFLICT`
  - `STATE_CONFLICT`
  - `INTERNAL_ERROR`
  - `TIMEOUT`
- 明确 create / commit / delete 的重复请求、幂等冲突和状态冲突行为。
- 明确 `DeleteMetadataRecord` 的 deleted-again 语义：
  - 同 `request_id` 再删为 `IDEMPOTENT_REPLAY`
  - 不同 `request_id` 再删为 `NOT_FOUND`
- 明确 `HeadMetadataRecord` / `ListMetadataRecords` 的 committed-only visibility。
- 明确 follower 上 `Head/List` 必须返回 `NOT_LEADER` 和 leader hint，不能返回本地 stale metadata。
- 明确响应字段 `request_id`、`object_key`、`state`、`term`、`log_index`、`leader_id`、`leader_address`、`message` 的语义。
- 明确 `payload` 是 metadata-only，不承载真实大文件 bytes。
- 删除了“实现阶段需固定”这类已过期开放表述。

## 修改文件

- 已修改：`specs/005-strong-consistency-metadata-layer/api.md`
- 已新增：`specs/005-strong-consistency-metadata-layer/task-reports/t035-report.md`

## 验证

- 本次为文档任务，未执行构建验证。

## 验收结论

- `T035`：已完成本次范围内文档回填。
- 未进入 `T036`。
