# T029 执行报告

## 任务范围

- 任务编号：`T029`
- 任务目标：在 `apps/raft_metadata_client.cpp` 中增加 Metadata Client 的 create generator，用于生成模拟 `object_key`、`object_size`、`chunk_size`、`chunk_count`、`checksum`、`mock_locations`、`payload` 并发起 `CreateMetadataRecord` 请求。
- 本次仅处理：
  - `apps/raft_metadata_client.cpp`
- 本次未执行：
  - `T030` 及后续任务
  - CMake target 接入
  - 完整 T031 command dispatcher
  - T032 read-after-write verification mode
  - StorageNode / ChunkStore / 真实文件与真实 chunk 逻辑

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `apps/AGENTS.md`
  - `apps/raft_kv_client.cpp`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
- 为对齐当前客户端已有实现，最小读取了：
  - `apps/raft_metadata_client.cpp`
  - `proto/raft.proto`
  - `modules/raft/common/metadata_command.h`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t025-report.md`
- 未修改 CMake，未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 新增最小 `create` 子命令

在现有 `raft_metadata_client.cpp` 的最小客户端骨架上新增了 `create` 子命令，但没有把客户端扩展成完整 dispatcher。

当前支持的命令集合为：

- `create`
- `commit-retry`
- `delete-retry`

### 2. create generator 参数支持

`create` 现在支持：

- `--request-id`
- `--object-key`
- `--object-size`
- `--chunk-size`
- `--chunk-count`（可选）
- `--checksum`（可选）
- `--mock-location`（可重复传入）
- `--payload`（metadata-only）
- `--timeout-ms`

### 3. 自动生成行为

在未显式提供某些字段时，客户端会生成 mock 值：

- 若未提供 `chunk_count`：
  - 自动按 `ceil(object_size / chunk_size)` 计算
- 若未提供 `checksum`：
  - 生成 mock checksum，格式为 `sha256:mock:<object_key>:<object_size>:<chunk_size>:<chunk_count>`
- 若未提供 `mock_locations`：
  - 自动生成 `mock-node-N/chunk-i` 形式的位置字符串

这些字段都只是 metadata control plane 的模拟值：

- 不读取真实文件
- 不计算真实文件 checksum
- 不生成真实 chunk
- 不检查 mock location 指向的真实节点或路径是否存在

### 4. create RPC 与输出

客户端会构造 `CreateMetadataRecordRequest` 并发起 RPC。

输出中明确包含：

- `request_id`
- `object_key`
- `state`
- `object_size`
- `chunk_size`
- `chunk_count`
- `checksum`
- `mock_locations`
- `status`
- `leader_id`
- `leader_address`
- `term`
- `log_index`

并明确打印：

- `payload_kind=metadata-only`
- `payload_bytes=<size>`

### 5. 边界保持

本次实现没有越界到以下内容：

- 没有实现完整 create / commit / delete / head / list dispatcher
- 没有修改 MetadataService / proto / state_machine / common command
- 没有接入 CMake target
- 没有实现真实大文件上传下载

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

- `T029`：通过本次范围内实现与回归验收

说明：

- 当前只补了 create generator，没有进入 `T030`
- 按用户约束，未修改 `tasks.md`
