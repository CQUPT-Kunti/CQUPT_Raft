# T002 Store Types Test

## 修改文件

- `tests/store_types_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/plan.md`
- `specs/007-storage-node-data-plane/task-reports/t002-store-types-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 为 `modules/store/common/store_types.h` 的最小占位类型新增基础测试入口 `tests/store_types_test.cpp`。
- 测试只覆盖 T001 已有占位内容：`ChunkLocation::IsValid()` 和 `StoreModuleStage::kPlaceholder`。
- 在 `tests/CMakeLists.txt` 中新增 `store_types` CTest 入口，并挂上 `storage-node;platform-neutral` 标签。
- 修正了 `tasks.md` 中 T001/T002/T003/T007/T008/T009 的文件路径、测试文件名和 T002 验收命令，使其与当前 `modules/store/common` 落点一致，并将 T002 标记为完成。
- 修正了 `plan.md` 中仍使用 `modules/raft/storage_node` 的路径说明，改为 `modules/store/` 下按 `common/` 与 `storage_node/` 分层。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
- 结果：PASS
- `ctest --test-dir build/linux -R "storage_node_types|store_types" --output-on-failure`
- 结果：PASS

## 是否通过 T002

- 是

## 是否可以进入 T003

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍返回 `specs/006-remove-kv-metadata-state-machine`，与 007 不一致。
- 当前 `tests/CMakeLists.txt` 中多处 `gtest_discover_tests(... PROPERTIES LABELS "...;...")` 写法在本地实际只暴露首个标签；本次 T002 已绕开该问题，但后续测试分组仍需统一处理。

## 是否修正了高频文档，为什么

- 是。
- 修改了 `specs/007-storage-node-data-plane/tasks.md`，因为 T001/T002/T007/T008/T009 的文件路径、测试文件名与当前 `modules/store/common/store_types.*` 和 `tests/store_types_test.cpp` 的实际落点不一致，且 T002 完成后需要按任务流程标记完成状态。
- 修改了 `specs/007-storage-node-data-plane/plan.md`，因为其中仍写 `modules/raft/storage_node`，与当前 `modules/store/` 的路径约定冲突。

## common-risk-notes.md 新增/删除/解决了哪些项

- 删除了 T001 中关于 `plan.md/tasks.md` 仍使用旧 StorageNode 路径的风险项，因为本次已完成必要修正。
- 保留了 `.specify` prerequisites 脚本仍指向 006 的风险项。
- 新增了一个关于 `gtest_discover_tests` 多标签在当前写法下只暴露首个标签的公共风险项。
