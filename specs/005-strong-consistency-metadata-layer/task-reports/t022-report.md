# T022 执行报告

## 任务范围

- 任务编号: `T022`
- 任务目标: 在 `modules/raft/service/metadata_service_impl.cpp` 中实现 `DeleteMetadataRecord` service adapter。
- 本次未执行: `T023` 及后续任务；未修改 `metadata_state_machine.*`、`metadata_command.*`、`metadata_result.h`、`proto/raft.proto`、测试文件。

## 实现结果

- 已在 `metadata_service_impl.cpp` 中新增 `MakeDeleteMetadataCommand()`，将 `DeleteMetadataRecordRequest` 转换为 metadata delete command。
- 已实现 `MetadataServiceImpl::DeleteMetadataRecord()`：
  - 先做请求校验；
  - 通过 `SerializeMetadataCommand(command)` + `node_.ProposeMetadata(...)` 走 Raft proposal 路径提交；
  - 使用现有 `FillWriteSummary()` 回填 `request_id`、`object_key`、`status`、`state=Deleted`、`term`、`log_index`、`leader hint`。
- `NOT_LEADER`、`INVALID_ARGUMENT`、`NOT_FOUND`、`STATE_CONFLICT`、`IDEMPOTENT_REPLAY`、`IDEMPOTENCY_CONFLICT` 等结果语义沿用现有 `ProposeResult -> MetadataStatusCode` 映射透传。
- service 层未保存 `records_`、`tombstones_`、`replay_table_` 等生命周期状态；仅负责适配、proposal 和响应填充。
- 未改动 create / commit / head / list 的既有行为。

## 验证结果

- 执行命令: `cmake --preset debug-ninja-low-parallel`
- 结果: `PASS`

- 执行命令: `cmake --build --preset debug-ninja-low-parallel --target raft_demo`
- 结果: `PASS`

- 执行命令: `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot)Test'`
- 结果: `PASS`
- 摘要: metadata 相关既有测试共 20 项，全部通过，无回退。

## 验收结论

- `T022` 当前实现已满足本次范围内的核心要求：DeleteMetadataRecord 已接入 service adapter，并通过 Raft proposal 路径提交，响应包含必要的 metadata 写请求摘要字段。
- 当前还没有 delete service 专项测试；本次验收依据为 `raft_demo` 构建通过以及既有 metadata 测试无回退。
- 本次不进入下一步；`T023` 及后续任务保持未执行。
