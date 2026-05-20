# T034 执行报告

## 任务范围

- 任务编号：`T034`
- 任务目标：新增 Metadata Client 场景测试，覆盖 `raft_metadata_client` 的 mock manifest、`create/commit/head/list/delete`、`verify-read-after-write`、重复请求、payload boundary、`mock_locations` 行为。
- 本次仅处理：
  - `tests/metadata_client_scenario_test.cpp`
  - `tests/CMakeLists.txt`
- 本次未执行：
  - `T035` 及后续任务
  - 客户端业务逻辑修改
  - 文档更新任务

## 读取与边界确认

- 已先读取根目录 `NOTREAD.md`，并遵守禁止路径约束。
- 按任务允许范围读取了：
  - `apps/raft_metadata_client.cpp`
  - `specs/005-strong-consistency-metadata-layer/client-design.md`
- 为对齐测试接入与风格，最小读取了：
  - `tests/CMakeLists.txt`
  - `tests/metadata_command_test.cpp`
  - `tests/metadata_manifest_test.cpp`
  - `tests/test_kv_service.cpp`
  - `tests/metadata_failover_test.cpp`
  - `proto/raft.proto`
  - `CMakeLists.txt`
- 未修改 `apps/raft_metadata_client.cpp`，未修改 `MetadataService` / proto / state_machine / common command。
- 未读取 `specs/004-raft-industrialization/**`，未全量扫描 `tests/**`。

## checklist 状态

| Checklist | Total | Completed | Incomplete | Status |
|-----------|-------|-----------|------------|--------|
| requirements.md | 16 | 16 | 0 | PASS |

## 实现内容

### 1. 新增客户端场景测试

新增 `tests/metadata_client_scenario_test.cpp`，测试通过 CMake 构建出的 `raft_metadata_client` 可执行文件发起真实进程调用，而不是直接复用客户端内部函数。

测试里起了一个本地 fake `MetadataService`，用最小的 in-memory 语义驱动客户端场景：

- 只暴露 gRPC Metadata API
- 不依赖真实 Raft 集群
- 不读取真实文件
- 不生成真实 chunk
- 不访问真实 StorageNode / ChunkStore

### 2. 覆盖场景

本次新增 5 个场景用例：

1. `CreateScenarioBuildsMetadataOnlyManifest`
   - 验证 `create` 会生成 metadata-only manifest
   - 验证 `payload_kind=metadata-only`
   - 验证 `mock_locations` 可接受不存在的节点/路径

2. `CreateCommitHeadListDeleteFlowSucceeds`
   - 覆盖 `create -> commit -> head -> list -> delete -> head`
   - 验证基本流程与可见性切换

3. `VerifyReadAfterWriteModeReportsPass`
   - 覆盖 `verify-read-after-write`
   - 验证 create 后不可见、commit 后可见、delete 后不可见

4. `DuplicateRequestIdDoesNotCreateDuplicateVisibleRecord`
   - 覆盖重复 `request_id`
   - 验证不会产生重复可见记录

5. `PayloadBoundaryAndMockLocationsBehaviorAreExposed`
   - 覆盖合法 payload
   - 覆盖超限 payload
   - 覆盖 `mock_locations` 不要求真实 StorageNode

### 3. CMake 接入

在 `tests/CMakeLists.txt` 中新增 `test_metadata_client_scenario` target，并：

- 依赖 `raft_metadata_client`
- 通过编译定义把 `$<TARGET_FILE:raft_metadata_client>` 传给测试
- 使用 `gtest_discover_tests(...)` 接入 CTest

### 4. 覆盖边界说明

本次场景测试的覆盖方式是：

- 用 fake `MetadataService` 验证客户端参数解析、请求构造、进程输出和子命令流程
- 不启动完整 Raft 服务端或真实 metadata cluster

因此本次重点覆盖的是：

- 客户端进程入口
- 命令分发
- mock manifest 生成
- 稳定输出字段
- 基于 Metadata API 的读后写验证流程

不是：

- 真实 leader failover 下的客户端场景联调
- 真实数据面 / StorageNode 交互

## 修改文件

- 新增：`tests/metadata_client_scenario_test.cpp`
- 修改：`tests/CMakeLists.txt`

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client
cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario
ctest --test-dir build/linux --output-on-failure -R '^MetadataClientScenarioTest\.'
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target raft_metadata_client`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario`：PASS
- `ctest --test-dir build/linux --output-on-failure -R '^MetadataClientScenarioTest\.'`：PASS
  - 共 `5/5` 通过
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test'`：PASS
  - 共 `34/34` 通过
  - 无 metadata 相关回退

### 说明

- 本次没有使用 standalone 编译或 standalone 运行代替 CMake/CTest 最终验证。
- 本次没有修改客户端业务逻辑；测试依赖的是已经接入 CMake 的 `raft_metadata_client` 可执行目标。

## 验收结论

- `T034`：通过本次范围内实现与回归验收

说明：

- 已补齐 Metadata Client 场景测试
- 当前不进入 `T035`
