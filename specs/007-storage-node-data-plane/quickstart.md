# Quickstart: Storage Node Data Plane

**Feature**: `007-storage-node-data-plane`  
**Status**: Plan phase only.

## Current Status

- 当前只完成 `/speckit-plan` 文档规划。
- StorageNode 尚未实现。
- `LocalDiskChunkStore` 尚未实现。
- `WriteChunk` / `ReadChunk` / `DeleteChunk` 尚未实现。
- 本阶段没有修改生产源码、真实 proto、CMake、测试实现或脚本。
- 不要把下面命令理解为当前已经可验证 StorageNode；它们是后续实现后的预期验证入口。

## Future Build Entry

后续实现完成后，优先使用项目既有低并发 preset：

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

更保守时：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## Future Linux Validation Entry

StorageNode high-concurrency tests 可并行：

```bash
./test.sh --group storage
```

durability/recovery/snapshot/catch-up/crash 边界测试保持低并发：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

Linux 专项矩阵后续应覆盖：

- fsync/fdatasync；
- directory sync；
- disk full；
- permission denied；
- partial write；
- stale staging cleanup；
- atomic publish；
- checksum mismatch；
- crash recovery；
- restart index rebuild；
- high-concurrency chunk IO。

## Future Windows Validation Entry

后续实现后，Windows 入口应使用项目既有 PowerShell/CMakePresets 路径，具体 test filter 在 `/speckit-tasks` 阶段定义。

Windows 专项矩阵后续应覆盖：

- `FlushFileBuffers`；
- Windows file handle；
- `MoveFileEx` / `ReplaceFile` publish 语义；
- Windows long path；
- UTF-8 path；
- disk full；
- permission denied；
- partial write；
- staging cleanup；
- atomic publish；
- checksum mismatch；
- restart index rebuild。

## Future Cross-Platform Durability Checklist

后续进入 `/tasks` 时，必须把以下检查拆成具体任务或测试项：

| Check | Linux | Windows |
|-------|-------|---------|
| chunk data flush | `fsync` / `fdatasync` | `FlushFileBuffers` |
| publish | same-filesystem rename | `MoveFileEx` / `ReplaceFile` |
| parent directory | directory sync | explicit supported/weaker/unsupported contract |
| path | UTF-8 + normalized relative path | UTF-8 + Windows long path + reserved names |
| errors | disk full / permission denied / IO error | disk full / access denied / sharing violation / IO error |
| recovery | stale staging / partial write / corrupted quarantine | stale staging / partial write / corrupted quarantine |
| index | restart index rebuild | restart index rebuild |
| concurrency | high-concurrency chunk IO | high-concurrency chunk IO |

## Future Functional Validation Flow

上传闭环：

1. `CreateObject` 创建 pending metadata。
2. coordinator/client 将对象切分 chunk。
3. Placement 选择 3 副本目标。
4. 并发 `WriteChunk` 到 StorageNode。
5. 每个 chunk 至少 2 个 durable success。
6. `CommitObject` 写入 `ChunkRef` manifest。
7. `HeadObject` / `ListObjects` 可见 committed 对象。

读取闭环：

1. 先读 metadata。
2. 读取 committed `ObjectRecord.chunks`。
3. 按 offset 排序。
4. 选择健康 `replica_nodes`。
5. `ReadChunk`。
6. checksum on read。
7. 副本失败 fallback。

删除闭环：

1. `DeleteObject` 提交 tombstone/DELETED。
2. 读路径立即不可见。
3. 后台 GC 调用 `BatchDeleteChunks` / `DeleteChunk`。
4. 重复 delete 和 StorageNode 重启后清理保持幂等。

## Future No-KV Audit

no-KV audit 仍需保留，后续实现不得恢复：

- `CommandType::kSet`
- `CommandType::kDelete`
- `KvStateMachine`
- `KvService`
- `raft_kv_client`
- `DebugGetValue`
- KV proto
- KV target
- KV fallback
- KV regression-only path
- `SetCommand` / `DeleteCommand` / KV 状态机断言

后续可通过既有 audit 入口或 CTest target 验证 no-KV surface，具体命令在 tasks 阶段落到真实测试计划中。

## Test Log Rule Reminder

后续真实运行测试时：

- 通过只报命令、PASS、总耗时。
- 失败只报失败测试名、关键断言、失败分类、最后 50 行日志和完整日志路径。
- 不在聊天中输出完整 Raft 节点日志。
