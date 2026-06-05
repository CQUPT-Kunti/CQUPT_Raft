# T049 Delete GC Safety

## 修改文件

- `tests/storage_delete_gc_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t049-delete-gc-safety.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_delete_gc_test.cpp`
- 在测试内实现 test-only cleanup candidate builder、committed live manifest safety check 和 cleanup apply helper
- 覆盖 metadata-first 删除闭环、live manifest 保护、重复删除重放、重复 cleanup 尝试幂等，以及 failed upload orphan cleanup candidate 边界
- 在 `tests/CMakeLists.txt` 注册 `test_storage_delete_gc` / `storage_delete_gc`

## 删除闭环 / GC safety 覆盖场景

- `DeleteObject` 后 `HeadObject` / `ListObjects` 立即不可见
- committed object 删除后，chunk 只作为 test-only cleanup candidate 处理，再由 test-only helper 调用本地 `DeleteChunk`
- 删除后 `MetadataStateMachine::FindChunkRefs(...)` 不再暴露 manifest，测试使用删除前捕获的 committed manifest 生成 cleanup candidate
- committed live manifest 仍引用同一个 chunk fact 时，cleanup 会被 metadata-driven safety check 阻止，不会误删共享 chunk
- 相同 `DeleteObject` 命令重放保持 metadata 幂等边界
- 重复 cleanup 尝试保持幂等，第二次删除返回 `already_missing`
- failed upload / pending object 的 durable orphan chunk 可以成为 cleanup candidate，但对象仍保持 metadata 不可见

## test-only helper 与生产 GC 当前边界

- 本任务只新增测试侧 helper，不实现生产 `GarbageCollector`
- helper 只做：
  - 从删除前 manifest 或 upload `cleanup_candidates` 构造测试候选
  - 扫描当前 committed live manifest 判断 chunk 是否受保护
  - 在安全时直接调用 `LocalDiskChunkStore::DeleteChunk`
- helper 不做：
  - 后台队列
  - restart 后继续 cleanup
  - `DeleteChunk` / `BatchDeleteChunks` RPC
  - repair / rebalance / scrub
  - registry / heartbeat / failure cache

## 是否使用 tests/test_file/test_file.deb

- 是

## node-data 可视化目录路径和保留内容（如适用）

- 未使用

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存
- `ctest --test-dir build/linux -R "storage_delete_gc|delete_gc" --output-on-failure 2>&1 | tee tmp/007/t049-delete-gc.log`
  - PASS
  - 日志路径：`tmp/007/t049-delete-gc.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证最终 PASS，无失败项

## Windows 验证判断

- T049 当前只在 Linux 环境验证删除/GC safety 测试
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T049-WIN`

## 是否通过 T049

- 是

## 是否可以进入 T050

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- `DeleteObject` 后 metadata 不再暴露 chunk manifest，因此 cleanup candidate 只能来自删除前捕获的 committed manifest 或 upload 失败结果；这符合当前 contract，但也说明生产 GC 仍需单独的 task model / queue / durable boundary
- T049 只固定 test-only safety contract，不关闭真实 `DeleteChunk` RPC、生产 `GarbageCollector`、restart cleanup、registry / heartbeat、timeout/cancellation、corrupted 自动回写、Windows 待验证等风险

## 是否更新 module-notes.md / AGENTS.md

- 未更新 `module-notes.md`
- 未更新 `AGENTS.md`

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 不需要
- 本任务未修改 `modules/store/*` 生产代码

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T049 标记完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T049 风险条目，明确当前只有 test-only delete/GC safety helper，生产删除/GC 仍待后续任务

## common-risk-notes.md 读取结果

- 已读取
- 重点确认以下风险仍保留：
  - T027 pending / orphan cleanup 未解决
  - T045 registry / failure cache 未接入
  - T024 corrupted 自动回写未实现
  - T019 / T020 timeout/cancellation 运行中传播未实现
  - Windows 待验证
  - restart rebuild / staging cleanup 未实现

## common-risk-notes.md 新增/删除/保留情况

- 新增：T049
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045
- 变更：无
