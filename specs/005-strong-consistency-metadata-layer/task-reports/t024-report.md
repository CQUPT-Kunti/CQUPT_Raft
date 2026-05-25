# T024 执行报告

## 任务范围

- 任务编号：`T024`
- 任务目标：确保 `MetadataService` 的 `HeadMetadataRecord` / `ListMetadataRecords` 读路径是 leader-safe 的。
- 本次仅处理：
  - `modules/raft/service/metadata_service_impl.cpp`
- 本次未执行：
  - `T025` 及后续任务
  - 客户端 failover retry
  - 新增测试文件
  - 状态机 / snapshot / tombstone 新逻辑

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按 T024 范围读取了：
  - `specs/005-strong-consistency-metadata-layer/tasks.md`
  - `modules/raft/service/metadata_service_impl.cpp`
  - `modules/raft/service/kv_service_impl.cpp`
  - `modules/raft/node/raft_node.h`
- 为最小对齐现有逻辑，额外读取了：
  - `modules/raft/service/metadata_service_impl.h`
- 未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`，未读取运行数据目录。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 现状核对

核对结果显示，`MetadataServiceImpl::HeadMetadataRecord` 与 `MetadataServiceImpl::ListMetadataRecords` 已在进入 metadata 状态机查询前调用本地 helper 进行 leader 判定：

- 通过 `RaftNode::GetStatusSnapshot()` 获取 `role`、`leader_id`、`leader_address`
- 若当前节点不是 leader，则直接返回 `NOT_LEADER`
- 只有 leader 才继续调用 `StrongConsistencyMetadataStateMachine` 的 committed-only 查询接口

因此当前实现本身已经满足 T024 的核心 leader-only read 约束，不存在 follower 先读本地 metadata 状态机再返回结果的路径。

### 本次最小修改

在 `modules/raft/service/metadata_service_impl.cpp` 中对读路径 helper 做了一个必要小修：

- 将 `EnsureLeaderForRead(...)` 扩展为接收 `object_key`
- 在 `HeadMetadataRecord` 返回 `NOT_LEADER` 时，把请求中的 `object_key` 一并写入 `MetadataResponseSummary`

修改目的：

- 保持 `NOT_LEADER` 响应更可诊断
- 不改变 `Head/List` 的 leader-only 行为
- 不改变 `create / commit / delete` 写路径语义
- 不改动状态机 committed-only visibility 语义

## 修改文件

- 已修改：`modules/raft/service/metadata_service_impl.cpp`
- 未修改：
  - `modules/raft/node/raft_node.h/.cpp`
  - `modules/raft/state_machine/metadata_state_machine.h/.cpp`
  - `modules/raft/common/metadata_command.h/.cpp`
  - `modules/raft/common/metadata_result.h`
  - `proto/raft.proto`
  - `tests/**`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target raft_demo
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo`：PASS
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'`：PASS
  - 共 `22/22` 通过
  - 无 metadata 相关回退

## 验收结论

- `T024`：通过

说明：

- 当前 `Head/List` 只允许 leader 继续执行 metadata 查询
- follower 会返回 `NOT_LEADER` 与 `leader hint`
- 本次未进入 `T025`
- 按用户约束，未修改 `tasks.md`
