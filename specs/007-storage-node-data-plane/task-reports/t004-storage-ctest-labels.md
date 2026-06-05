# T004 Storage CTest Labels

## 修改文件

- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/t004-storage-ctest-labels.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 在 `tests/CMakeLists.txt` 中补充了 StorageNode / store 数据面标签契约说明，显式预留：
  - `storage-node`
  - `storage-node-concurrency`
  - `storage-node-recovery`
  - `storage-node-cross-platform`
  - `platform-neutral`
- 新增 `RAFT_STORE_LABELS_BASELINE`、`RAFT_STORE_LABELS_CONCURRENCY`、`RAFT_STORE_LABELS_RECOVERY`、`RAFT_STORE_LABELS_CROSS_PLATFORM` 标签组合变量，供后续 007 storage tests 复用。
- 新增 `add_store_ctest(...)` helper，统一用套件级 `add_test` 为 store / storage tests 挂标签，绕开 `gtest_discover_tests` 多标签暴露不完整的问题。
- 将现有 `store_types` 入口改为使用 `add_store_ctest(...)` 的基线标签组合，确保至少可被 `storage-node` 和 `platform-neutral` 稳定筛选。
- 将 `tasks.md` 中 T004 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
- 结果：PASS
- `ctest --test-dir build/linux -N -L storage-node`
- 结果：PASS，列出 `store_types`
- `ctest --test-dir build/linux -N -L platform-neutral`
- 结果：PASS，列出 `store_types` 及现有 platform-neutral 测试
- `ctest --test-dir build/linux -R "store_types" --output-on-failure`
- 结果：PASS
- `ctest --test-dir build/linux -N -L storage-node-concurrency`
- 结果：PASS，当前 0 个测试
- `ctest --test-dir build/linux -N -L storage-node-recovery`
- 结果：PASS，当前 0 个测试
- `ctest --test-dir build/linux -N -L storage-node-cross-platform`
- 结果：PASS，当前 0 个测试

## 是否使用 CodeGraph / 是否修改 codegraph.db

- 未使用 CodeGraph。
- 未修改 `codegraph.db`。

## 是否通过 T004

- 是

## 是否可以进入 T005

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍返回 `specs/006-remove-kv-metadata-state-machine`，与 007 不一致。

## 是否修正了高频文档，为什么

- 是。
- 修改了 `specs/007-storage-node-data-plane/tasks.md`，原因是本任务完成后需要按任务流转状态将 T004 标记为完成；未向高频文档追加执行日志或流水记录。

## common-risk-notes.md 新增/删除/解决了哪些项

- 删除了 T002 中关于 `gtest_discover_tests` 多标签暴露不完整会阻塞后续 storage-node 测试分组的风险项，因为本次已为 store / storage tests 引入独立的套件级标签注册方式。
- 删除了 T003 中关于 `.codegraph/codegraph.db-shm` 修改状态的风险项，因为当前工作区已无该变更。
- 未新增公共风险。
- 保留了 `.specify` prerequisites 指向 006 的风险项。
