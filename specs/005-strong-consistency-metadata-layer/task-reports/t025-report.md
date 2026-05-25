# T025 执行报告

## 任务范围

- 任务编号：`T025`
- 任务目标：在客户端侧补充 failover retry 场景，使 `CommitMetadataRecord` / `DeleteMetadataRecord` 在 `NOT_LEADER` 或 `TIMEOUT` 后使用同一 `request_id` 重试，并显示 leader hint。
- 本次仅处理：
  - `apps/raft_metadata_client.cpp`
- 本次未执行：
  - `T026` 及后续任务
  - 完整 metadata command dispatcher
  - create generator
  - read-after-write verification mode
  - 真实文件 / chunk / StorageNode 相关逻辑

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `apps/AGENTS.md`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
  - `proto/raft.proto`
  - `modules/raft/common/metadata_result.h`
  - `modules/raft/common/metadata_command.h`
- `apps/raft_metadata_client.cpp` 在开始时不存在，因此本次按任务允许范围新建该文件。
- 未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`，未读取运行数据目录。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 新建最小 metadata failover retry 客户端

新增 `apps/raft_metadata_client.cpp`，但只覆盖 T025 所需的最小场景，不实现完整 dispatcher。

当前支持两个子命令：

- `commit-retry`
- `delete-retry`

### 2. request_id 复用与有限重试

实现了有限次数 retry 逻辑：

- CLI 参数要求显式传入 `--request-id`
- 当响应码为 `NOT_LEADER` 或 `TIMEOUT` 时，客户端不会生成新 `request_id`
- retry 始终复用原始 `request_id`
- 通过 `--max-retries` 控制最大重试次数，避免无限循环

### 3. leader hint 与诊断输出

客户端会输出并展示：

- `request_id`
- `object_key`
- `code`
- `message`
- `state`
- `leader_id`
- `leader_address`
- `term`
- `log_index`

当响应中包含 `leader_hint.leader_address` 时：

- 优先显示 leader hint
- retry 时优先切换到 hint 提供的地址

### 4. 保持边界

本次实现没有引入以下越界内容：

- 没有实现完整 metadata CLI command dispatcher
- 没有实现 create generator
- 没有读取真实文件
- 没有生成真实 chunk
- 没有访问 Raft 内部日志、snapshot 或测试内部状态
- 没有修改 service / proto / state_machine / common command

## 修改文件

- 新增：`apps/raft_metadata_client.cpp`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client
cmake --build --preset debug-ninja-low-parallel --target raft_demo
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client`：FAIL
  - 失败原因：当前工程中不存在该 target
  - 关键错误：`ninja: error: unknown target 'raft_metadata_client'`
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo`：PASS
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'`：PASS
  - 共 `22/22` 通过
  - 无 metadata 相关回退

### 说明

- 本次无法正式构建 metadata client，待 `T033` 接入 CMake target。
- 按任务要求，本次没有使用 standalone 编译作为最终验证。

## 验收结论

- `T025`：通过本次范围内实现与回归验收

说明：

- 已提供最小的 failover retry 客户端文件
- retry 场景复用同一 `request_id`
- 当前不进入 `T026`
- 按用户约束，未修改 `tasks.md`
