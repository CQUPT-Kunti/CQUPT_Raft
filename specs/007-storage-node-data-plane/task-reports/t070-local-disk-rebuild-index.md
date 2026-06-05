# T070 Local Disk Rebuild Index

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.h`
- `modules/store/chunk/local_disk_chunk_store.cpp`
- `modules/store/chunk/module-notes.md`
- `tests/storage_node_recovery_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t070-local-disk-rebuild-index.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在生产 `LocalDiskChunkStore` 中新增 `RebuildIndexFromDisk()`，并让 `Initialize()` 在目录准备完成后自动执行 live index rebuild。
- 恢复流程只扫描 canonical `chunks/live/` final chunk，基于本地磁盘 payload 事实恢复 `chunk_id`、`object identity`、`size`、`checksum`、`state=LIVE`。
- 重建前会清空现有 `ChunkIndex`，保证恢复只依赖本地磁盘事实，不信任旧内存索引。
- 新测试改为直接走生产 `Initialize()` / `RebuildIndexFromDisk()`，固定 T068 contract 在生产实现上的落地行为。

## RebuildIndexFromDisk 输入、输出和恢复语义

- 输入：
  - 已初始化的 `data_root/chunks/live/`
  - 当前 store 持有的 `ChunkIndex`
- 输出：
  - 只包含 canonical live final chunk 的本地 `ChunkIndex`
- 恢复语义：
  - 只扫描 `chunks/live/`
  - 只恢复 `ChunkState::kLive`
  - 恢复 `chunk_id` / `object_id` / `version` / `chunk_index` / `size` / `checksum`
  - 不决定 object committed/deleted 可见性
  - 不调用 metadata / Raft
  - 不保存 payload 到 metadata / Raft

## live/staging/malformed/duplicate/corrupted 当前边界

- live：
  - canonical final chunk 会重建进 index
- staging：
  - `chunks/staging/` 和 partial staging 不进入 live index
- malformed：
  - 非 `.chunk`、非法 `chunk_id`、misplaced live candidate 安全跳过
- duplicate：
  - 同一 `chunk_id` 出现多个 live candidate 时返回明确 `kConflict`
- corrupted：
  - 当前只基于 live payload bytes 计算恢复 checksum
  - 不做 quarantine / corrupted 状态持久化或自动迁移
  - 这部分仍留给 T072

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata / Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)` 和小型手写 payload

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_recovery|local_disk_chunk_store|rebuild_index|restart_index" --output-on-failure 2>&1 | tee tmp/007/t070-local-disk-rebuild-index.log`
  - PASS
  - 实际匹配到的测试名为 `local_disk_chunk_store`、`storage_node_recovery`
  - 日志路径：`tmp/007/t070-local-disk-rebuild-index.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮实现主要使用现有 `std::filesystem` 路径遍历和既有 durable path 规则
- Windows live directory traversal / path behavior 仍待后续实机验证

## 是否通过 T070

- 是

## 是否可以进入 T071

- 可以
- T071 应继续做 stale staging cleanup / partial write detection，不要把 T070 扩展成 quarantine 或 crash matrix

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 live final 文件仍只有 payload bytes，没有 tombstone / quarantine sidecar；因此 deleted/quarantined/corrupted 状态不能仅靠 live 文件自描述恢复。
- 当前恢复只重建 live index，不处理 stale staging cleanup。
- 当前 Windows 目录遍历、long path / UTF-8 path 和路径编码语义仍未实机验证。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `module-notes.md`
- 未更新 `AGENTS.md`

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - live directory scan helper
  - chunk filename parse helper
  - final chunk validation helper
  - checksum/size recovery helper
  - ChunkIndex entry rebuild helper
  - duplicate / malformed handling helper
  - `RebuildIndexFromDisk()` 主流程

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T070 完成，并把验收描述收缩到实际的 live rebuild contract
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：关闭“生产 rebuild 未实现”这一层，并保留 T071/T072/Windows 后续风险

## common-risk-notes.md 读取结果

- 已读取
- 既有 Windows durability / delete / concurrency 风险仍保留
- corrupted/quarantine、stale staging cleanup、crash matrix 风险仍保留
- prerequisites 脚本指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T070`，记录生产 rebuild 已落地，但恢复仍只覆盖 live final chunk，Windows 路径遍历仍待验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T068/T069` 等后续风险继续保留
- 收缩：
  - `T021` 从“rebuild 未实现”收缩为“stale staging cleanup / 非 live 恢复治理未实现”
