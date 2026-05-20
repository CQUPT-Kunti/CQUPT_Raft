# T032 执行报告

## 任务范围

- 任务编号：`T032`
- 任务目标：在 `apps/raft_metadata_client.cpp` 中增加客户端 read-after-write verification mode，验证 create 后不可见、commit 后可见、delete 后不可见。
- 本次仅处理：
  - `apps/raft_metadata_client.cpp`
- 本次未执行：
  - `T033` 及后续任务
  - CMake target 接入
  - 真实文件 / chunk / StorageNode / ChunkStore 相关逻辑

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `apps/AGENTS.md`
  - `apps/raft_metadata_client.cpp`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
- 为对齐协议与上一任务实现，最小读取了：
  - `proto/raft.proto`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t031-report.md`
  - `specs/005-strong-consistency-metadata-layer/task-reports/t029-report.md`
- 仅最小查看了 `tasks.md` 中 `T032/T033` 行用于范围确认。
- 未修改 `tasks.md`，未修改 CMake，未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 新增 read-after-write verification mode

在 `apps/raft_metadata_client.cpp` 中新增显式子命令：

- `verify-read-after-write`

该模式只通过 `MetadataService` API 串起完整验证流程，不直接访问 Raft 内部日志、snapshot 或状态机对象。

### 2. 验证流程

验证模式会按以下顺序执行：

1. `CreateMetadataRecord`
2. `HeadMetadataRecord`，验证 create 后不可见
3. `ListMetadataRecords`，验证 create 后不可见
4. `CommitMetadataRecord`
5. `HeadMetadataRecord`，验证 commit 后可见
6. `ListMetadataRecords`，验证 commit 后可见
7. `DeleteMetadataRecord`
8. `HeadMetadataRecord`，验证 delete 后不可见
9. `ListMetadataRecords`，验证 delete 后不可见

其中：

- `create` / `commit` / `delete` 的 request_id 由 `--request-id` 基础值自动派生为：
  - `<base>:create`
  - `<base>:commit`
  - `<base>:delete`
- `commit` 的 `expected_create_request_id` 自动复用 create 阶段 request_id
- `list` 默认使用 `object_key` 作为 prefix；如果显式传入 `--prefix`，则按用户值执行

### 3. 复用已有请求构造能力

本次没有新写一套独立 RPC 流程，而是复用了 T031/T029 已有能力：

- `DoCreate`
- `DoCommit`
- `DoDelete`
- `DoHead`
- `DoList`

另外抽取并复用了：

- create manifest 生成
- head/list 响应打印
- 稳定 summary 输出

### 4. 诊断输出

每个校验步骤都会输出：

- `request_id`
- `object_key`
- `status`
- `message`
- `leader_id`
- `leader_address`
- `term`
- `log_index`
- `expected`
- `actual`
- `result=PASS|FAIL`

如果某一步失败：

- 会先输出对应 RPC/summary 诊断
- 再输出 `verification_check`
- 明确说明期望值与实际值
- 立即返回非 0

### 5. 边界保持

本次实现没有越界到以下内容：

- 没有修改 dispatcher 之外的协议语义
- 没有修改 `MetadataService` / proto / state_machine / common command
- 没有接入 `raft_metadata_client` CMake target
- 没有读取真实文件
- 没有生成真实 chunk
- 没有访问 StorageNode 或 ChunkStore
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

- `T032`：通过本次范围内实现与回归验收

说明：

- 已补齐客户端 read-after-write verification mode
- 验证模式只调用 `MetadataService` API
- 当前不进入 `T033`
