# T021 执行报告

## 任务范围

- 任务编号: `T021`
- 任务目标: 新增 tombstone 和 snapshot/restart 单元测试，覆盖 `VM-011`、`VM-012`、`VM-013`。
- 本次未执行: `T022` 及后续任务；未修改 MetadataService / proto / client；未修改 `metadata_state_machine.cpp`。

## 实现结果

- 已新增 `tests/metadata_snapshot_test.cpp`。
- 已在 `tests/CMakeLists.txt` 中最小接入 `test_metadata_snapshot` target，并通过 `gtest_discover_tests()` 纳入 `CTest`。
- 新增测试覆盖：
  - committed metadata 经 `SaveSnapshot` / `LoadSnapshot` 后仍可被 `Head/List` 查询；
  - Deleted tombstone 经恢复后继续不可见；
  - Pending 记录经恢复后仍不可见；
  - tombstone 恢复后，同一 delete request_id、旧 create request_id、旧 commit request_id 的重放仍返回可解释结果，且对象不会复活；
  - 新 create 请求在 tombstone 恢复后仍被阻止；
  - 损坏 snapshot、magic mismatch、version mismatch 均返回明确错误。
- 测试使用临时 snapshot 文件，不依赖 `raft_data/**`、`raft_snapshots/**`、真实 chunk 文件或 StorageNode。

## 验证结果

- 执行命令: `cmake --preset debug-ninja-low-parallel`
- 结果: `PASS`

- 执行命令: `cmake --build --preset debug-ninja-low-parallel --target test_metadata_snapshot`
- 结果: `PASS`

- 执行命令: `ctest --test-dir build/linux --output-on-failure -R '^MetadataSnapshotTest\\.'`
- 结果: `PASS`
- 摘要: `MetadataSnapshotTest` 共 5 项，全部通过。

- 执行命令: `ctest --test-dir build/linux --output-on-failure -R 'Metadata(StateMachine|Snapshot)Test'`
- 结果: `PASS`
- 摘要: `MetadataStateMachineTest` + `MetadataSnapshotTest` 共 11 项，全部通过，无回退。

## 验收结论

- `T021` 当前实现已满足本次范围内的核心要求：覆盖 tombstone、snapshot/restart 恢复、Pending 恢复不可见、replay table 恢复和损坏 snapshot 错误路径。
- 本次不进入下一步；`T022` 及后续任务保持未执行。
