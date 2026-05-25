# T036 执行报告

## 任务范围

- 任务编号：`T036`
- 任务目标：回填 `specs/005-strong-consistency-metadata-layer/data-model.md` 中已经固定的字段约束、payload 上限、tombstone 保留策略和恢复模型。
- 本次仅处理：
  - `specs/005-strong-consistency-metadata-layer/data-model.md`
- 本次未执行：
  - `T037` 及后续任务
  - 任意源码修改
  - 任意测试修改
  - 任意构建或测试命令

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守其中禁止路径。
- 按任务与用户约束，重点读取了：
  - `specs/005-strong-consistency-metadata-layer/tasks.md`
  - `specs/005-strong-consistency-metadata-layer/data-model.md`
- 为对齐当前固定语义，最小补充读取了：
  - `specs/005-strong-consistency-metadata-layer/api.md`
  - `specs/005-strong-consistency-metadata-layer/plan.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t018-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t019-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t020-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t027-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t028-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t035-report.md`

## 文档回填内容

- 将 `data-model.md` 从规划表述收口为当前固定的数据模型语义。
- 明确 `MetadataManifest` 字段约束：
  - `object_size > 0`
  - `chunk_size > 0`
  - `chunk_count > 0`
  - `chunk_count == ceil(object_size / chunk_size)`
  - `checksum` 非空且非纯空白
  - `mock_locations` 非空，且每个 entry 非空
- 明确 `payload` 只允许 metadata-only 小字段，且上限为 `4096` 字节。
- 明确 `MetadataRecordState` 只包含 `Pending`、`Committed`、`Deleted`，并固定 committed-only visibility。
- 明确 `MetadataRecord`、`IdempotencyEntry`、`replay_table`、`Tombstone` 的字段和作用。
- 明确 tombstone retention：删除事实必须保留，用于阻止旧 create/commit 复活对象。
- 明确 snapshot/restart 必须恢复 committed metadata、tombstone 和必要 replay 状态。
- 明确当前阶段不引入 `StorageNode`、`ChunkStore`、真实 chunk 或数据面模型。
- 删除了“实现阶段需固定”这类已过期开放语句。

## 修改文件

- 已修改：`specs/005-strong-consistency-metadata-layer/data-model.md`
- 已新增：`specs/005-strong-consistency-metadata-layer/task-reports/t036-report.md`

## 验证

- 本次为文档任务，未执行构建验证。

## 验收结论

- `T036`：已完成本次范围内文档回填。
- 未进入 `T037`。
