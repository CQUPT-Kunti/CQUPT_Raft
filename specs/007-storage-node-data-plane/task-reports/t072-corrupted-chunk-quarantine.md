# T072 Corrupted Chunk Quarantine

## 修改文件

- `modules/store/chunk/local_disk_chunk_store.h`
- `modules/store/chunk/local_disk_chunk_store.cpp`
- `modules/store/chunk/module-notes.md`
- `tests/local_disk_chunk_store_test.cpp`
- `tests/storage_node_recovery_test.cpp`
- `tests/storage_read_integration_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t072-corrupted-chunk-quarantine.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `LocalDiskChunkStorePaths` 中加入 `quarantine_root`，初始化时创建 `chunks/quarantine/`。
- 在 `ReadChunk()` / `StatChunk(verify_checksum=true)` 中，当 final chunk 的 size/checksum 与 index metadata 不一致时：
  - 返回明确错误；
  - 将 canonical live final chunk 移入 canonical quarantine 路径；
  - 把 `ChunkIndex` 中该 entry 的状态更新为 `ChunkState::kQuarantined`。
- 扩展 `RebuildIndexFromDisk()`，除 `chunks/live/` 外，还扫描 `chunks/quarantine/`，把已持久化的 quarantine 事实恢复进 `ChunkIndex`，但不把它们当成 healthy live。
- 补充测试，固定 read-time quarantine、restart 恢复 quarantine、以及读集成 fallback 到健康副本的 contract。

## corrupted chunk quarantine 输入、输出和状态语义

- 输入：
  - `ChunkIndex` 中的 `LIVE` entry
  - `chunks/live/` final chunk 文件
  - `ReadChunk()` / `StatChunk(verify_checksum=true)` 的实际 size/checksum 校验结果
  - `chunks/quarantine/` 上已有的本地目录事实
- 输出：
  - 坏块从 live 路径移动到 canonical quarantine 路径
  - `ChunkIndexEntry.state` 更新为 `kQuarantined`
  - `ReadChunk()` / `StatChunk(verify_checksum=true)` 返回明确错误
  - `RebuildIndexFromDisk()` 恢复 `kLive` 与 `kQuarantined` 两类本地事实
- 状态语义：
  - `kLive`：可读、可参与正常 read replica
  - `kQuarantined`：不可读，只能作为本地损坏事实观察到，不能作为健康副本
  - 本轮没有新增 repair/rebalance/scrub 语义，也没有把坏块物理删除

## rebuild/read/stat/list 当前边界

- rebuild：
  - 扫描 `chunks/live/` 与 `chunks/quarantine/`
  - canonical live chunk 恢复为 `kLive`
  - canonical quarantine chunk 恢复为 `kQuarantined`
  - duplicate rebuild candidate 仍返回明确 `kConflict`
  - malformed / misplaced candidate 继续安全跳过
  - 不能仅凭 `chunks/live/*.chunk` payload bytes 主动发现“此前未标记”的 checksum mismatch
- read：
  - `kQuarantined` / `kCorrupted` 直接拒读
  - 读 live final file 时若发现 size/checksum 与 index metadata 不一致，返回明确错误并 quarantine
  - `expected_checksum` 与实际 payload 不一致仍只返回 `kChecksumMismatch`，不把 caller 约束失配当成 store 本地坏块
- stat：
  - `verify_checksum=false` 时可观察到 `kQuarantined`
  - `verify_checksum=true` 时若发现坏块，返回明确错误并 quarantine
- list：
  - 默认不返回 quarantine
  - `include_quarantine=true` 时可观察到 `kQuarantined`

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata / Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)` 和已有 fixture helper

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_recovery|local_disk_chunk_store|storage_read|corrupted|quarantine" --output-on-failure 2>&1 | tee tmp/007/t072-corrupted-chunk-quarantine.log`
  - PASS
  - 实际匹配到的测试名为 `local_disk_chunk_store`、`storage_node_recovery`、`storage_read_integration`、`storage_read_chunk_contract`
  - 日志路径：`tmp/007/t072-corrupted-chunk-quarantine.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮 quarantine 使用 `std::filesystem::rename`/目录遍历路径
- Windows rename/delete/sharing violation/path encoding 实机行为仍待后续验证

## 是否通过 T072

- 是

## 是否可以进入 T073

- 可以
- T073/T074 继续处理 crash matrix，不要把本轮 quarantine 收口误当成 crash durability 已完成

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 quarantine 的主动发现入口只在前台 `ReadChunk()` / `StatChunk(verify_checksum=true)`；纯重启扫描无法独立发现“从未被标记”的 live checksum mismatch。
- 当前恢复只重建 live/quarantine 本地事实，不解决 deleted/deleting 更完整持久状态恢复。
- Windows rename/delete/sharing violation 仍未实机验证。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `module-notes.md`
- 未更新 `AGENTS.md`

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - regular file scan helper
  - quarantine directory scan helper
  - quarantine path / state helper
  - rebuild-time quarantine helper
  - read/stat-time quarantine helper
  - `RebuildIndexFromDisk()` 的 live/quarantine 恢复边界

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T072 完成并写明真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：删除“坏块只返回错误不隔离”的旧风险，保留主动发现、Windows 和 crash matrix 后续风险

## common-risk-notes.md 读取结果

- 已读取
- Windows durability / delete / rename / sharing violation 风险仍保留
- crash matrix、metadata freshness、主动 scrub/repair 风险仍保留
- prerequisites 脚本指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T072`，记录 quarantine 仍依赖前台读/查触发，Windows rename/delete 仍待验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T068/T069/T070/T071` 等后续风险继续保留
- 收缩：
  - `T021` 从“corrupted/quarantine 未实现”收缩为“主动发现、metadata freshness、后台治理仍未实现”
  - `T024` 从“坏块只返回错误不隔离”收缩为“已有前台 quarantine，但尚无 scrub/failure cache/facts 传播”
