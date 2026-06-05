# T003 No-KV Audit Store Coverage

## 修改文件

- `tests/no_kv_surface_audit.cmake`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/t003-no-kv-audit-store-coverage.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 在 `tests/no_kv_surface_audit.cmake` 中补充了 `modules/store/` 的显式覆盖断言，确保 store 数据面源码仍纳入 production strict scan。
- 为未来 `proto/storage_node.proto` 增加了覆盖与构建注册检查：文件存在时，必须被 production scan 覆盖，且根 `CMakeLists.txt` 中要有对应注册痕迹。
- 为 storage 相关测试入口增加了覆盖与注册检查：`tests/store*_test.cpp`、`tests/storage*_test.cpp`、`tests/local_disk_chunk_store_test.cpp` 及 `tests/support/store_*`、`tests/support/storage_*` 会被纳入 no-KV scan，并要求 `tests/CMakeLists.txt` 中存在对应注册痕迹。
- 增加了对旧 `modules/raft/storage_node` 路径和旧 `storage_node_types_test.cpp` 测试入口的严格禁止检查。
- 将 `tasks.md` 中 T003 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
- 结果：PASS
- `ctest --test-dir build/linux -R "store_types" --output-on-failure`
- 结果：PASS

## 是否使用 CodeGraph / 是否修改 codegraph.db

- 未使用 CodeGraph。
- 未修改 `codegraph.db`。
- 但当前工作区存在 `.codegraph/codegraph.db-shm` 修改状态，不属于本任务实现内容，提交时应排除。

## 是否通过 T003

- 是

## 是否可以进入 T004

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍返回 `specs/006-remove-kv-metadata-state-machine`，与 007 不一致。
- 工作区存在 `.codegraph/codegraph.db-shm` 修改状态，后续提交时需要注意排除。

## 是否修正了高频文档，为什么

- 是。
- 修改了 `specs/007-storage-node-data-plane/tasks.md`，原因是本任务完成后需要按任务流转状态将 T003 标记为完成；未向高频文档追加执行日志或流水记录。

## common-risk-notes.md 新增/删除/解决了哪些项

- 新增了 1 条 T003 风险：`.codegraph/codegraph.db-shm` 处于修改状态，可能污染业务提交。
- 未解决或删除既有风险项：`.specify` prerequisites 指向 006 的问题仍存在；`gtest_discover_tests` 多标签暴露不完整的问题仍存在。
