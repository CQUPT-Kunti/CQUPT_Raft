# T031 执行报告

## 任务范围

- 任务编号：`T031`
- 任务目标：在 `apps/raft_metadata_client.cpp` 中实现统一 Metadata Client command dispatcher，支持 `create`、`commit`、`delete`、`head`、`list` 子命令，并保留 `commit-retry` / `delete-retry`。
- 本次仅处理：
  - `apps/raft_metadata_client.cpp`
- 本次未执行：
  - `T032` 及后续任务
  - read-after-write verification mode
  - CMake target 接入
  - 真实文件 / chunk / StorageNode / ChunkStore 相关逻辑

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `apps/AGENTS.md`
  - `apps/raft_kv_client.cpp`
  - `proto/raft.proto`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
- 为对齐当前实现，最小读取了：
  - `apps/raft_metadata_client.cpp`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t025-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t029-report.md`
- 未修改 `tasks.md`，未修改 CMake，未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 统一 command dispatcher

在 `apps/raft_metadata_client.cpp` 中把客户端入口收敛为统一 dispatcher，当前支持：

- `create`
- `commit`
- `delete`
- `head`
- `list`
- `commit-retry`
- `delete-retry`

入口层只负责参数解析、请求构造、RPC 调用和结果输出，没有引入 metadata 生命周期状态。

### 2. create / commit / delete / head / list 子命令

- `create`
  - 保留 `T029` 的 create generator
  - 继续支持 `object_key`、`object_size`、`chunk_size`、可选 `chunk_count`、可选 `checksum`、可重复 `mock_location`、`payload`
  - 输出明确标注 `payload_kind=metadata-only`
- `commit`
  - 构造并发送 `CommitMetadataRecord`
  - 支持 `expected_create_request_id` 和 `commit_info`
- `delete`
  - 构造并发送 `DeleteMetadataRecord`
  - 支持 `delete_info`
- `head`
  - 构造并发送 `HeadMetadataRecord`
  - 输出 `found` 和命中的 record 摘要
- `list`
  - 构造并发送 `ListMetadataRecords`
  - 支持 `prefix`、`limit`、`page_token`
  - 输出 `prefix`、返回记录数量、`next_page_token` 和每条记录摘要

### 3. 保留 failover retry 语义

- `commit-retry` / `delete-retry` 继续存在
- `NOT_LEADER` 或 `TIMEOUT` 时复用同一个 `request_id`
- 根据 `leader_hint.leader_address` 选择下一次 retry 地址
- 重试次数仍由 `--max-retries` 限制，不会无限循环

### 4. 稳定输出字段

所有子命令的 summary 都保持稳定输出以下字段：

- `request_id`
- `object_key`
- `status`
- `message`
- `leader_id`
- `leader_address`
- `term`
- `log_index`

其中：

- `head` 额外输出 `found`
- `list` 额外输出 `prefix`、`records_count`、`next_page_token`
- record 摘要输出 manifest 与 request_id 相关字段，但不输出真实文件内容

### 5. 边界保持

本次实现没有越界到以下内容：

- 没有实现 `T032` 的 read-after-write verification mode
- 没有读取真实文件
- 没有计算真实文件 checksum
- 没有生成真实 chunk
- 没有绕过 `MetadataService` 直接访问状态机
- 没有修改 `MetadataService` / proto / state_machine / common command
- 没有接入 `raft_metadata_client` CMake target

## 修改文件

- 已修改：`apps/raft_metadata_client.cpp`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target raft_demo
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo`：PASS
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover)Test'`：PASS
  - 共 `29/29` 通过
  - 无 metadata 相关回退

### 说明

- `raft_metadata_client` target 待 `T033` 接入，因此本次无法通过项目 CMake target 正式构建该 client。
- 按任务要求，本次没有使用 standalone 编译作为最终验证。

## 验收结论

- `T031`：通过本次范围内实现与回归验收

说明：

- 已实现统一 metadata client dispatcher
- 已保留 `commit-retry` / `delete-retry` 同 `request_id` 重试语义
- 当前不进入 `T032`
