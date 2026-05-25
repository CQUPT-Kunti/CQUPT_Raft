# T023 执行报告

## 任务范围

- 任务编号：`T023`
- 任务目标：新增 metadata leader failover 集成测试，验证 leader failover 后：
  - `Committed` metadata 仍然可见；
  - `Pending` metadata 仍然不可见；
  - 为后续跨 leader `request_id` retry 场景建立验证基础。
- 本次未执行：`T024`、`T025` 及后续任务。

## 读取范围说明

- 已先读取项目根目录 `NOTREAD.md`，并按约束避开禁止路径。
- 按任务允许范围读取：
  - `specs/005-strong-consistency-metadata-layer/tasks.md`
  - `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - `proto/raft.proto`
  - `tests/CMakeLists.txt`
  - `modules/raft/service/metadata_service_impl.h`
  - `modules/raft/node/raft_node.h`
  - `modules/raft/state_machine/metadata_state_machine.h`
- 为构造最小 failover 集成测试，额外最小读取了少量直接相关测试文件：
  - `tests/raft_integration_test.cpp`
  - `tests/test_kv_service.cpp`
- 读取上述两个测试文件的目的仅限：
  - 复用三节点集群启动 / leader 选举等待模式；
  - 对齐现有 gRPC stub 调用与端口分配方式；
  - 避免自行猜测测试基建接口。
- 未全量扫描 `tests/**`。

## 修改文件

- 新增：`tests/metadata_failover_test.cpp`
- 修改：`tests/CMakeLists.txt`
- 未修改：
  - `metadata_state_machine.h/.cpp`
  - `metadata_service_impl.h/.cpp`
  - `proto/raft.proto`
  - 其他业务实现文件

## 实现内容

### 1. 新增 `MetadataFailoverTest`

新增 `tests/metadata_failover_test.cpp`，构造最小三节点 metadata 集成测试场景，直接启动 `RaftNode` 集群并通过 `MetadataService` gRPC stub 发起请求。

测试覆盖：

1. `NewLeaderKeepsCommittedVisibleAndPendingHidden`
   - 在旧 leader 上提交一条 `create + commit` metadata；
   - 再创建一条仅 `create`、未 `commit` 的 `Pending` metadata；
   - 关闭旧 leader，等待新 leader 产生；
   - 在新 leader 上验证：
     - `Committed` 记录可通过 `HeadMetadataRecord` / `ListMetadataRecords` 查询；
     - `Pending` 记录仍不可见；
     - `List` 只暴露已提交对象。

2. `SameCommitRequestIdCanBeRetriedOnNewLeader`
   - 在旧 leader 上完成 `create + commit`；
   - 关闭旧 leader，等待新 leader；
   - 用相同 `commit request_id` 向新 leader 重试；
   - 验证重试结果稳定、对象不重复、可见状态保持 `Committed`。

### 2. 最小接入 CMake

在 `tests/CMakeLists.txt` 中新增：

- `test_metadata_failover`

仅做测试 target 接入，没有改动已有 target 行为，也没有跳过、删除或重命名任何现有测试。

## 结果说明

- 已满足 `VM-014`：
  - leader failover 后，已提交 metadata 在新 leader 侧仍可通过 `Head/List` 查询。
- 已满足 `VM-015`：
  - `Pending` metadata 在 failover 后仍不对外暴露。
- 已为后续跨 leader `request_id` retry 场景提供基础集成验证：
  - 相同 `commit request_id` 在新 leader 上重试后不会产生重复可见记录。

## 验证

### 执行命令

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target test_metadata_failover
ctest --test-dir build/linux --output-on-failure -R '^MetadataFailoverTest\.'
ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'
```

### 验证结果

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_failover`：PASS
- `ctest --test-dir build/linux --output-on-failure -R '^MetadataFailoverTest\.'`：PASS
  - `MetadataFailoverTest` 2/2 通过
- `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine|Snapshot|Failover)Test'`：PASS
  - `MetadataCommandTest`、`MetadataStateMachineTest`、`MetadataSnapshotTest`、`MetadataFailoverTest` 共 22/22 通过

### 额外说明

- failover 集成测试需要本地 gRPC server 绑定回环端口；在沙箱内运行时端口绑定受限，因此验证阶段使用了允许本地端口绑定的执行方式完成 CTest。
- 本次未进入 `T024` 的 leader-safe read，也未进入 `T025` 的 client failover retry。

## 验收结论

- `T023`：通过
- 当前不进入下一步；`T024` 及后续任务保持未执行。
