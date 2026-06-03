# T071 Stale Staging Cleanup

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.h`
- `modules/store/chunk/local_disk_chunk_store.cpp`
- `modules/store/chunk/module-notes.md`
- `tests/storage_node_recovery_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t071-stale-staging-cleanup.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `LocalDiskChunkStoreConfig` 中新增 `staging_cleanup_grace_period_ms`，默认 5 分钟。
- 在 `Initialize()` 中接入 recovery cleanup 主流程：目录准备完成后，先扫描 `chunks/staging/` 并清理超过阈值的 stale / partial staging，再执行 T070 的 live index rebuild。
- cleanup 失败时，`Initialize()` 返回明确错误，不 silent success。
- 新增恢复测试，固定 stale/partial 删除、fresh 保留、live 不受影响，以及 cleanup failure 的显式错误行为。

## stale staging cleanup 输入、输出和清理语义

- 输入：
  - `paths_.staging_root`
  - `staging_cleanup_grace_period_ms`
  - staging 目录下的文件/目录 `last_write_time`
- 输出：
  - 超过阈值的 staging file 被删除
  - 删除后变空的 staging 子目录被 prune
  - `ChunkIndex` 仍只由 `chunks/live/` final chunk rebuild
- 清理语义：
  - 只处理 `chunks/staging/`
  - stale / partial staging 由 `mtime + grace period` 判断
  - fresh staging 不删除
  - malformed staging 文件只要位于 staging 且已过阈值，也作为 staging 垃圾清理
  - cleanup 失败直接阻断 `Initialize()`

## fresh/stale/partial/live 当前边界

- fresh：
  - 阈值内 staging 保留，不进入 live index
- stale：
  - 超过阈值的 staging file 会删除
- partial：
  - 遗留在 staging 中的 partial / incomplete file 超过阈值后会删除
- live：
  - `chunks/live/` final chunk 不参与 cleanup
  - cleanup 后 live rebuild 仍只基于 canonical final chunk

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata / Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)` 和手写小 payload

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_recovery|local_disk_chunk_store|stale_staging|staging_cleanup" --output-on-failure 2>&1 | tee tmp/007/t071-stale-staging-cleanup.log`
  - PASS
  - 实际匹配到的测试名为 `local_disk_chunk_store`、`storage_node_recovery`
  - 日志路径：`tmp/007/t071-stale-staging-cleanup.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮实现使用 `std::filesystem` 遍历、`last_write_time` 和目录删除
- Windows sharing violation / directory delete / path encoding 实机行为仍待验证

## 是否通过 T071

- 是

## 是否可以进入 T072

- 可以
- T072 应继续做 corrupted chunk quarantine，不要把 T071 扩展成 crash matrix

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 staging cleanup 依赖 `last_write_time`，mtime 精度和未来时间戳仍是恢复边界。
- 当前 cleanup 只解决 stale/partial staging，不解决 corrupted/quarantine。
- 当前 Windows sharing violation、路径编码和目录删除行为仍未实机验证。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `module-notes.md`
- 未更新 `AGENTS.md`

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - staging directory scan helper
  - stale staging 判断 helper
  - cleanup threshold / mtime 判断 helper
  - partial staging cleanup helper
  - safe remove helper
  - `Initialize()` / recovery cleanup 主流程

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T071 完成，并把验收描述收缩到实际的 stale staging cleanup contract
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：关闭 stale staging cleanup 这一层，并保留 T072/T073/T074/Windows 后续风险

## common-risk-notes.md 读取结果

- 已读取
- Windows durability / delete / concurrency 风险仍保留
- corrupted/quarantine、crash matrix、metadata freshness 风险仍保留
- prerequisites 脚本指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T071`，记录 mtime 精度、Windows sharing violation / directory delete 仍待后续验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T068/T069/T070` 等后续风险继续保留
- 收缩：
  - `T021` 从“stale staging cleanup 未实现”收缩为“corrupted/quarantine 与 metadata freshness 未实现”
