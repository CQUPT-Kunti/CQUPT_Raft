# T005 Store Test Utils

## 修改文件

- `tests/support/store_test_utils.h`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/t005-store-test-utils.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 新建 `tests/support/store_test_utils.h`，提供 header-only 的 store / storage 测试辅助能力。
- helper 覆盖了后续 store 测试最常用的轻量能力：
  - 测试名清洗
  - 跨平台临时根目录与唯一测试目录生成
  - `ScopedStoreTestDir` 自动清理目录
  - `StorageNodeId` / `ChunkId` / `ChunkLocation` fixture
  - 固定模式的 chunk payload 生成
  - 简单的 fixture checksum 字符串生成
- 将 `tasks.md` 中 T005 的文件名从旧的 `tests/support/storage_node_test_utils.h` 修正为更符合当前路径约定的 `tests/support/store_test_utils.h`，并将 T005 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
- 结果：PASS
- `ctest --test-dir build/linux -R "store_types" --output-on-failure`
- 结果：PASS
- `c++ -std=c++20 -I modules -I tests -x c++ -fsyntax-only tests/support/store_test_utils.h`
- 结果：PASS（仅有 `#pragma once in main file` 警告）

## 是否使用 CodeGraph / 是否修改 codegraph.db

- 未使用 CodeGraph。
- 本任务未修改 `codegraph.db`。
- 但当前工作区存在 `.codegraph/codegraph.db`、`.codegraph/codegraph.db-shm`、`.codegraph/codegraph.db-wal` 修改状态，不属于本任务实现内容，提交时应排除。

## 是否通过 T005

- 是

## 是否可以进入 T006

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍返回 `specs/006-remove-kv-metadata-state-machine`，与 007 不一致。
- 工作区存在 `.codegraph/` 索引文件修改状态，后续提交时需要注意排除。

## 是否修正了高频文档，为什么

- 是。
- 修改了 `specs/007-storage-node-data-plane/tasks.md`，因为 T005 仍使用旧文件名 `storage_node_test_utils.h`，与当前 `modules/store/` / `store_*` 的路径约定不一致；同时本任务完成后需要按任务流转状态将 T005 标记为完成。

## common-risk-notes.md 新增/删除/解决了哪些项

- 新增了 1 条 T005 风险：`.codegraph/` 索引文件处于修改状态，可能污染业务提交。
- 未删除既有风险项。
