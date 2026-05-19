# T027 执行报告

## 任务范围

- 任务编号：`T027`
- 任务目标：在 `metadata_command.cpp` 中强化 manifest boundary validation，确保 metadata command 只描述模拟 manifest 和 metadata-only payload，不接收真实大文件数据，也不依赖真实 StorageNode / 本地文件路径。
- 本次仅处理：
  - `modules/raft/common/metadata_command.cpp`
- 本次未执行：
  - `T028` 及后续任务
  - 测试文件新增或修改
  - 状态机 / service / proto / client 修改
  - StorageNode / ChunkStore 相关实现

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `modules/raft/common/metadata_command.cpp`
  - `modules/raft/common/metadata_command.h`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
- 为编译和回归验证最小读取了：
  - `modules/raft/common/metadata_result.h`
  - `tests/metadata_command_test.cpp`
- 未全量扫描 `tests/**`，未读取 `specs/004-raft-industrialization/**`，未读取运行数据目录。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

本次只在 `ValidateCreateRecord(...)` 中强化了 create manifest 边界校验，没有修改 codec 格式，也没有引入任何文件 IO 或 StorageNode 依赖。

新增 / 强化的校验规则：

1. `object_size` 必须大于 0
   - 拒绝零大小 object，避免无意义或不完整 manifest 进入 Raft metadata 命令层。

2. `chunk_size` / `chunk_count` 必须为正
   - 保持既有正数约束。

3. `chunk_count` 必须与 `object_size` / `chunk_size` 一致
   - 使用 `ceil(object_size / chunk_size)` 的等价整数公式校验：
     - `expected_chunk_count = 1 + ((object_size - 1) / chunk_size)`
   - 如果不一致则拒绝。

4. `checksum` 不得为空，也不得只包含空白字符
   - 避免“看似存在、实际无内容”的 checksum。

5. `mock_locations` 不得为空
   - 保持既有约束。

6. `mock_locations` 中的每个 entry 不得为空串或纯空白
   - 只校验字符串本身是否为空，不访问、不解析、不验证任何真实路径或 StorageNode。

7. `payload` 继续受明确字节上限约束
   - 仍使用现有 `kMaxPayloadBytes = 4096`
   - 超限直接拒绝
   - 这保证 payload 只作为 metadata-only 小字段，不承载真实大文件 bytes

## 边界保持说明

- 未访问 `mock_locations` 指向的任何真实路径
- 未检查真实 StorageNode 是否存在
- 未引入文件系统调用
- 未引入 StorageNode / ChunkStore 类型依赖
- 未修改 create / commit / delete codec 包装格式

## 修改文件

- 已修改：`modules/raft/common/metadata_command.cpp`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target test_metadata_command
ctest --test-dir build/linux --output-on-failure -R '^MetadataCommandTest\.'
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_command`：PASS
- `ctest --test-dir build/linux --output-on-failure -R '^MetadataCommandTest\.'`：PASS
  - 共 `9/9` 通过
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'`：PASS
  - 共 `22/22` 通过
  - 无 metadata 相关回退

## 验收结论

- `T027`：通过

说明：

- 本次只强化 manifest boundary validation，没有进入 `T028` 测试新增任务
- 按用户约束，未修改 `tasks.md`
