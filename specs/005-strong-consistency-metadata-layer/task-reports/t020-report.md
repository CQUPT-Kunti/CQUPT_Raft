# T020 执行报告

## 任务范围

- 任务编号: `T020`
- 任务目标: 在 `modules/raft/state_machine/metadata_state_machine.cpp` 中实现 metadata snapshot save/load。
- 本次未执行: `T021` 及后续任务；未新增或修改测试文件；未实现 service/client；未修改 KV snapshot 格式。

## 实现结果

- 已实现 `SaveSnapshot()`，使用独立的 metadata snapshot magic/version，未复用或修改 KV snapshot 格式。
- snapshot 会保存 `records_`、`tombstones_` 和 `replay_table_`，覆盖 committed metadata、Deleted tombstone 与必要的 request_id replay 状态。
- 已实现 `LoadSnapshot()`，可恢复 metadata records、tombstones 和 replay entries。
- `LoadSnapshot()` 对损坏 header、magic 不匹配、version 不匹配、重复 key / request_id 等情况返回明确错误。
- `HeadMetadataRecord` / `ListMetadataRecords` 的 committed-only 语义未变，因此恢复后的 `Committed` 记录可见，`Pending` / `Deleted` 仍对外不可见。
- 恢复后的 `tombstones_` 和 `replay_table_` 会继续阻止 tombstoned object 被旧 create/commit 复活。
- `SaveSnapshot()` 执行真实文件写入、flush 和 rename，不是 no-op 成功。

## 验证结果

- 执行命令: `cmake --preset debug-ninja-low-parallel`
- 结果: `PASS`

- 执行命令: `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- 结果: `PASS`

- 执行命令: `ctest --test-dir build/linux --output-on-failure -R '^MetadataStateMachineTest\\.'`
- 结果: `PASS`
- 摘要: `MetadataStateMachineTest` 共 6 项，全部通过，无回退。

## 验收结论

- `T020` 当前实现已满足本次范围内的核心要求：独立 metadata snapshot 格式、committed metadata 恢复、Deleted tombstone 恢复、replay table 恢复，以及损坏 / magic / version 错误的明确返回。
- 由于本次未新增 snapshot/restart 专项测试，当前验收依据为实现自查和既有 `MetadataStateMachineTest` 无回退。
- 本次不进入下一步；`T021` 及后续任务保持未执行。
