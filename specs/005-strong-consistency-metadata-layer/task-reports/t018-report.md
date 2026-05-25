# T018 执行报告

## 任务范围

- 任务编号: `T018`
- 任务目标: 在 `modules/raft/state_machine/metadata_state_machine.cpp` 中实现 `Committed -> Deleted` 的 tombstone 状态转换。
- 本次未执行: `T019` 及后续任务；未实现 delete request_id 幂等；未实现 snapshot/restart；未实现 service delete adapter；未新增或修改测试文件。

## 实现结果

- 已在 `Apply()` 中新增 delete 分支，限制 `DeleteMetadataRecord` 只允许作用于 `Committed` 记录。
- 已在 delete 成功时把记录状态更新为 `MetadataRecordState::kDeleted`，并写入 `delete_request_id`、`deleted_at_log_index`、`delete_info`。
- 已同步写入 `tombstones_`，保留删除事实，未物理删除 tombstone 信息。
- 已保证 `Pending` 记录 delete 返回 `state confli    ct`，never-created 记录 delete 返回 `not found`。
- 已阻止 tombstoned key 被后续旧 `create` 重新创建；对 Deleted 记录的旧 `commit` 不会把状态重新变回 `Committed`。
- `HeadMetadataRecord` / `ListMetadataRecords` 仍仅暴露 `Committed` 记录，因此 Deleted 记录对外不可见。

## 验证结果

- 执行命令: `cmake --preset debug-ninja-low-parallel`
- 结果: `PASS`

- 执行命令: `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- 结果: `PASS`

- 执行命令: `ctest --test-dir build/linux --output-on-failure -R '^MetadataStateMachineTest\\.'`
- 结果: `PASS`
- 摘要: `MetadataStateMachineTest` 共 6 项，全部通过，无回退。

## 验收结论

- `T018` 当前实现已满足本次范围内的核心要求：`Committed -> Deleted`、tombstone 保留、Head/List 删除后不可见、Pending delete 冲突、never-created delete not found。
- 由于本次未新增 delete 专项测试，当前验收依据为实现自查和既有 `MetadataStateMachineTest` 无回退。
- 本次不进入下一步；`T019` 及后续任务保持未执行。
