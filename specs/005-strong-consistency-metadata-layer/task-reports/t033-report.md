# T033 执行报告

## 任务范围

- 任务编号：`T033`
- 任务目标：在 `CMakeLists.txt` 中新增 `raft_metadata_client` 可执行目标，将 `apps/raft_metadata_client.cpp` 正式接入项目构建。
- 本次仅处理：
  - `CMakeLists.txt`
- 本次未执行：
  - `T034` 及后续任务
  - client scenario tests
  - 客户端业务逻辑扩展

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `CMakeLists.txt`
  - `apps/raft_metadata_client.cpp`
- 为对齐现有 client target 写法，最小读取了：
  - `apps/raft_kv_client.cpp`
  - `proto/raft.proto`
  - `apps/AGENTS.md`
- 未修改 `apps/raft_metadata_client.cpp` 业务逻辑，未修改 `proto/raft.proto`，未修改 `tasks.md`。
- 未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 新增 `raft_metadata_client` target

在根 `CMakeLists.txt` 中新增：

- `add_executable(raft_metadata_client apps/raft_metadata_client.cpp)`
- `target_link_libraries(raft_metadata_client PRIVATE raft_proto)`

### 2. 依赖与兼容性

- 复用了项目已有 `raft_proto` 目标，因此自动获得 protobuf / gRPC 生成代码与头文件依赖。
- 没有改动 `raft_kv_client` target 名称、源文件或链接语义。
- 没有引入 Linux-only shell 语义，保持当前 CMake 写法与 Linux / Windows generator 兼容。

### 3. 边界保持

本次仅做 CMake wiring，没有进入以下范围：

- 没有修改 `apps/raft_metadata_client.cpp` 业务逻辑
- 没有修改 `MetadataService` / state_machine / common command
- 没有实现 `T034` 的 client scenario tests
- 没有实现真实文件、真实 chunk、StorageNode、ChunkStore 相关逻辑

## 修改文件

- 已修改：`CMakeLists.txt`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client
cmake --build --preset debug-ninja-low-parallel --target raft_demo
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_demo`：PASS
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover)Test'`：PASS
  - 共 `29/29` 通过
  - 无 metadata 相关回退

### 说明

- 本次 `raft_metadata_client` 已能通过项目 CMake target 正式构建，不再受 “待 T033 接入” 限制。
- 本次没有使用 standalone 编译代替 CMake 构建验证。

## 验收结论

- `T033`：通过本次范围内实现与回归验收

说明：

- 已正式接入 `raft_metadata_client` 可执行目标
- 当前不进入 `T034`
