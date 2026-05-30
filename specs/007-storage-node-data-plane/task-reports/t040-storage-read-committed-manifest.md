# T040 Storage Read Committed Manifest

## 修改文件

- `tests/storage_read_integration_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t040-storage-read-committed-manifest.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `storage_read_integration` 集成测试
- 在测试内实现最小 `ReadCommittedObjectByManifest(...)` 读取入口，固定读取顺序为：
  - 先 `HeadObject`
  - 只接受 `COMMITTED` object
  - 再读取 metadata manifest
  - 按 `ChunkRef.offset` 排序后逐块调用本地 `ChunkStore::ReadChunk()`
- 新增 `CountingChunkReader` 测试包装，统计 data-plane read 调用次数，固定 pending/deleted 场景不会触发 StorageNode/ChunkStore 读取
- 在 `tests/CMakeLists.txt` 注册 `test_storage_read_integration` / `storage_read_integration`

## committed manifest 读取测试覆盖场景

- committed object：
  - 使用 `tests/test_file/test_file.deb`
  - 将 payload 拆成多个 chunk
  - manifest 故意乱序提交
  - 读取时按 offset 顺序拼接，结果等于原始 payload
- metadata lookup first：
  - 测试入口固定先走 `HeadObject`
  - 不允许绕过 metadata 直接读 chunk
- pending object：
  - metadata 仅 `CreateObject`
  - 即使本地已存在 durable chunk，也返回不可见
  - `ChunkStore::ReadChunk()` 调用次数为 0
- deleted object：
  - `CreateObject -> CommitObject -> DeleteObject`
  - `HeadObject` / `ListObjects` 不可见
  - `ChunkStore::ReadChunk()` 调用次数为 0
- manifest / local chunk facts：
  - `replica_nodes`
  - `checksum`
  - `size`
  与本地读回的 chunk metadata 一致

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_read" --output-on-failure 2>&1 | tee tmp/007/t040-storage-read-test.log`
  - PASS
  - 日志路径：`tmp/007/t040-storage-read-test.log`

## Windows 验证判断

- 本任务只新增平台无关的 committed manifest 读取集成测试
- 当前验证仍通过 `LocalDiskChunkStore` 的 Linux 当前环境执行
- 未新增 `T040-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T040

- 是

## 是否可以进入 T041

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- 当前读取路径仍是测试内最小 helper，不是 T046 的通用测试 helper
- 当前没有 `StorageNodeService::ReadChunk` / `StorageNodeClient::ReadChunk` / 副本 fallback / 读副本选择，这些仍属于 T041-T047
- 当前读路径验证仍基于 `MetadataStateMachine` test adapter + `LocalDiskChunkStore`，不是“真实多 Raft 节点 + 真实 StorageNode Read RPC”的端到端测试
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`

## 是否更新 module-notes.md / AGENTS.md

- 未更新

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 不需要
- 本任务未修改 `modules/store/*` 生产代码

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T040 标记完成

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 变更：无
