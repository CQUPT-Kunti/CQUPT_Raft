# T028 执行报告

## 任务范围

- 任务编号：`T028`
- 任务目标：新增 manifest boundary 单元测试，验证 `T027` 中 metadata manifest 边界校验逻辑。
- 本次仅处理：
  - `tests/metadata_manifest_test.cpp`
  - `tests/CMakeLists.txt` 的最小测试接入
- 本次未执行：
  - `T029` 及后续任务
  - 源码业务逻辑修改
  - StorageNode / ChunkStore 实现
  - 客户端 / service / state_machine / proto 改动

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `modules/raft/common/metadata_command.h`
  - `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
- 为最小接入和对齐现有测试风格，额外最小读取了：
  - `tests/CMakeLists.txt`
  - `tests/metadata_command_test.cpp`
  - `modules/raft/common/metadata_command.cpp`
  - `modules/raft/common/metadata_result.h`
- 未全量扫描 `tests/**`，未读取 `specs/004-raft-industrialization/**`，未读取运行数据目录。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 新增 manifest boundary 单测

新增 `tests/metadata_manifest_test.cpp`，覆盖以下场景：

- 合法 manifest 可通过校验
- `chunk_size == 0` 被拒绝
- `chunk_count` 与 `object_size / chunk_size` 不匹配被拒绝
- `checksum` 缺失被拒绝
- `payload` 超过上限被拒绝
- `mock_locations` 为空被拒绝
- `mock_locations` 指向不存在节点或伪路径仍可接受

### 2. 保持 metadata-only 边界

测试中使用的 `mock_locations` 只作为普通字符串参与校验，不做任何真实路径访问，不检查节点是否存在，也不依赖真实 StorageNode 或 chunk 文件。

### 3. 最小 CMake 接入

在 `tests/CMakeLists.txt` 中新增：

- `test_metadata_manifest`

只做最小 target/wiring，没有修改、跳过、删除或重命名任何已有测试。

## 修改文件

- 新增：`tests/metadata_manifest_test.cpp`
- 修改：`tests/CMakeLists.txt`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target test_metadata_manifest
ctest --test-dir build/linux --output-on-failure -R '^MetadataManifestTest\.'
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_manifest`：PASS
- `ctest --test-dir build/linux --output-on-failure -R '^MetadataManifestTest\.'`：PASS
  - 共 `7/7` 通过
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover)Test'`：PASS
  - 共 `29/29` 通过
  - 无 metadata 相关回退

## 验收结论

- `T028`：通过

说明：

- 本次只补测试与最小 wiring，没有进入 `T029`
- 按用户约束，未修改 `tasks.md`
