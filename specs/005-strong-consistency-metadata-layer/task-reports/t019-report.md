# T019 执行报告

## 任务范围

- 任务编号: `T019`
- 任务目标: 在 `modules/raft/state_machine/metadata_state_machine.cpp` 中扩展 delete 的 request_id 幂等重放逻辑。
- 本次未执行: `T020` 及后续任务；未实现 snapshot/restart；未实现 DeleteMetadataRecord service adapter；未新增或修改测试文件。

## 实现结果

- 已在 `DeleteMetadataRecord` 成功路径中把 delete 结果写入 `replay_table_`。
- 同一 `request_id`、同一 `operation`、同一 `object_key`、同一 `fingerprint` 的重复 delete 现在会在 apply 入口命中既有 replay 逻辑，返回等价幂等结果。
- 同一 `request_id` 但 `operation`、`object_key` 或 `fingerprint` 不一致时，仍返回统一的 `idempotency conflict`。
- delete 成功时，`records_` 中记录状态、`tombstones_` 中 tombstone、`replay_table_` 中幂等条目在同一临界区内一起更新，避免重复 delete 产生不一致状态。
- 对已 Deleted 记录使用不同 delete request_id 的结果保持稳定，不会复活对象；旧 create/commit 请求也不会把 Deleted 重新变回 `Committed`。
- T010/T011 的 create/commit 幂等语义未改动；T018 的 tombstone 语义未回退。

## 验证结果

- 执行命令: `cmake --preset debug-ninja-low-parallel`
- 结果: `PASS`

- 执行命令: `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- 结果: `PASS`

- 执行命令: `ctest --test-dir build/linux --output-on-failure -R '^MetadataStateMachineTest\\.'`
- 结果: `PASS`
- 摘要: `MetadataStateMachineTest` 共 6 项，全部通过，无回退。

## 验收结论

- `T019` 当前实现已满足本次范围内的核心要求：delete request_id 重放、same request_id same fingerprint 幂等成功、same request_id different fingerprint 冲突、tombstone 与 replay table 一致更新。
- 由于本次未新增 delete 幂等专项测试，当前验收依据为实现自查和既有 `MetadataStateMachineTest` 无回退。
- 本次不进入下一步；`T020` 及后续任务保持未执行。
