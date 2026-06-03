# T068 Restart Index Rebuild Test

## 修改文件

- `tests/storage_node_recovery_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t068-restart-index-rebuild-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_node_recovery_test.cpp`，使用 test-only recovery scanner 固定 restart index rebuild 的最小 contract。
- scanner 只读取本地 `chunks/live/` 磁盘事实，重建 fresh `ChunkIndex`，不调用 metadata/Raft，也不决定 object committed/deleted 可见性。
- 测试覆盖：
  - 已 publish 的 final/live chunk 在“重启后”能被重建成 `ChunkState::kLive`
  - staging chunk 不进入 live index
  - partial staging / incomplete staging 不进入 live index
  - `deleted` / `deleting` / `quarantine` / `corrupted` 路径事实不被误当作 healthy live chunk
  - 恢复出的 `chunk_id` / `size` / `checksum` / `state` 可验证
  - 多个 final chunks 的 rebuild 顺序稳定
  - malformed / invalid live filename 被安全跳过
  - duplicate live chunk id across paths 返回明确 `kConflict`
  - zero-byte final chunk 可恢复为合法 live chunk
  - non-regular `.chunk` candidate 不进入 live index
- 在 `tests/CMakeLists.txt` 中新增 `storage_node_recovery` CTest 入口，并挂到 `storage-node-recovery` 标签。
- 更新 `tasks.md`：
  - 将 T068 标记为完成
  - 把后续 US5 recovery 测试路径统一为 `tests/storage_node_recovery_test.cpp`
- 更新 `common-risk-notes.md`，记录 T068 当前只固定 test-only rebuild contract，未关闭 T070-T072 的生产恢复风险。

## restart index rebuild contract 覆盖场景

- final live chunk 被扫描并重建为 live index entry
- staging chunk 不进入 live index
- partial staging 不进入 live index
- final chunk 的 `chunk_id`、`size`、`checksum`、`state` 可恢复
- 多个 final chunks 的 rebuild 顺序稳定
- malformed / invalid filename 安全跳过
- duplicate live chunk id 返回明确错误
- empty final file 作为 zero-byte live chunk 恢复
- non-regular `.chunk` candidate 不进入 live index
- rebuild 过程不调用 metadata/Raft

## test-only scanner/helper 与生产 RebuildIndexFromDisk 当前边界

- 当前 scanner 仅存在于测试文件中，用来固定未来生产 `LocalDiskChunkStore::RebuildIndexFromDisk` 必须满足的 contract。
- 当前没有修改 `modules/store/chunk/local_disk_chunk_store.h/.cpp`，没有提前实现 T070 生产扫描逻辑。
- 当前 scanner 只基于本地 `chunks/live/` 文件内容恢复 live facts，不处理 stale staging cleanup、corrupted quarantine、delete tombstone 持久化，也不把 object payload 写入 metadata/Raft。

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)` 构造小 payload

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_node_recovery|restart_index|rebuild_index" --output-on-failure 2>&1 | tee tmp/007/t068-storage-node-recovery.log`
  - PASS
  - 实际匹配到的测试名为 `storage_node_recovery`
  - 日志路径：`tmp/007/t068-storage-node-recovery.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T068 固定的是平台无关 restart index rebuild contract
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务没有引入 Windows 专属路径、句柄或 directory sync 语义，因此不新增 `T068-WIN`
- 当前判断：**Windows 待验证**

## 是否通过 T068

- 是

## 是否可以进入 T069

- 可以
- T069 应继续做 cross-platform durability matrix，不把 T068 扩成生产恢复实现

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 live final 文件只持久化 payload bytes；如果后续需要在“不调用 metadata/Raft”的前提下判定更强的 corrupted/deleted/quarantined 恢复事实，仍需在 T070-T072 明确 on-disk state encoding 或等价 contract。
- 当前 test-only scanner 只固定 live rebuild contract，不处理 stale staging cleanup、partial write quarantine、delete tombstone persistence。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只新增测试和任务/风险报告，没有修改 `modules/store/*` 生产代码

## module-notes.md 是否需要补充 .cpp 关键函数 / helper

- 否
- 当前新增 helper 全部位于测试文件内，不是生产 `.cpp` helper

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T068 完成，并把 US5 recovery 测试文件路径统一到实际新增的 `tests/storage_node_recovery_test.cpp`
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：记录 T068 仅固定 test-only rebuild contract，保留 T070-T072 仍需明确的恢复/状态编码风险

## common-risk-notes.md 读取结果

- 已读取并核对当前既有风险项
- 仍存在且本任务未关闭的风险包括：
  - prerequisites 脚本仍错误指向 006
  - Windows durability / delete / concurrency 仍待实机验证
  - timeout/cancellation 运行中传播未实现
  - corruption 自动状态回写未实现
  - registry freshness / failure cache 风险仍在
  - GC schema migration / 多进程 persistence_root 协议仍未定义

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T068`，记录当前 restart rebuild contract 只固定 test-only scanner 边界，生产 `RebuildIndexFromDisk` 仍需在 T070-T072 明确 on-disk state encoding 与 corruption/staging 处理
- 删除：
  - 无
- 保留：
  - 既有风险全部保留
