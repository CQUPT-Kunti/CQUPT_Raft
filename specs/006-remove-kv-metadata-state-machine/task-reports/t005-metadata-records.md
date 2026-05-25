# T005 Metadata 基础数据结构

## 1. T005 结论

- T005 已完成，范围仅限 metadata 基础 records、相关单测和必要的测试/CMake/AGENTS 同步。
- 本次未接入 `RaftNode`，未修改默认状态机装配，未删除任何 KV 代码。
- 新增的 records 位于现有 `modules/raft/common/metadata_command.h`，保持为轻量、头文件级、可序列化友好的基础结构。
- `BucketRecord`、`ObjectRecord`、`ChunkRef`、`RequestRecord` 已定义。
- `ObjectState` 已定义为 `PENDING / COMMITTED / DELETED`。
- `ChunkRef` 仅保存引用元数据，没有保存任何真实 chunk bytes。

## 2. 新增/修改文件

- 修改 `modules/raft/common/metadata_command.h`
- 新增 `tests/metadata_records_test.cpp`
- 修改 `tests/CMakeLists.txt`
- 修改 `modules/raft/common/AGENTS.md`
- 新增报告 `specs/006-remove-kv-metadata-state-machine/task-reports/t005-metadata-records.md`

## 3. Metadata 数据结构清单

- `ObjectState`
  - `PENDING`
  - `COMMITTED`
  - `DELETED`
- `MetadataRequestType`
  - `kCreateBucket`
  - `kDeleteBucket`
  - `kCreateObject`
  - `kCommitObject`
  - `kAbortObject`
  - `kDeleteObject`
- `ChunkRef`
  - 字段：`chunk_id`、`offset`、`size`、`replica_nodes`、`checksum`
  - 语义：只表达引用与放置信息，不保存真实数据
- `BucketRecord`
  - 字段：`bucket`、`create_time`、`deleted`、`delete_time`
- `ObjectRecord`
  - 字段：`bucket`、`object_key`、`object_id`、`version`、`size`、`etag`、`state`、`chunks`、`create_time`、`commit_time`、`delete_time`
  - helper：`IsPending()`、`IsCommitted()`、`IsDeleted()`
- `RequestRecord`
  - 字段：`request_id`、`command_type`、`bucket`、`object_key`、`result_status`、`applied_index`、`create_time`、`finish_time`
  - helper：`Finished()`
- 结构字段只使用 `std::string`、整数、`std::vector`、`std::optional`，未引入平台相关句柄，后续易于做序列化/快照落盘。
- 这些新结构不依赖 `KVCommand`、`KvStateMachine`、`KvService`。

## 4. CMake / AGENTS.md 更新情况

- `tests/CMakeLists.txt` 新增 `test_metadata_records` target。
- 未修改根 `CMakeLists.txt` 的业务 target 装配；现有 `raft_core` / `raft_demo` / `raft_kv_client` / `raft_metadata_client` 保持不变。
- `modules/raft/common/AGENTS.md` 已同步补充：
  - `metadata_command.h`
  - `metadata_command.cpp`
  - `metadata_result.h`
  - metadata 共享 records / command / result 的职责说明
  - `tests/metadata_records_test.cpp` 测试入口
- 未向 `AGENTS.md` 写入执行日志，只更新了模块文件位置与职责说明。

## 5. 测试结果

- Linux configure：
  - 命令：`cmake --preset debug-ninja-low-parallel`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t005-configure.log`
- Linux build：
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target test_metadata_command test_metadata_manifest test_metadata_records`
  - 结果：`PASS`
  - 覆盖：重新编译了 `raft_core` 受影响对象，以及 `test_metadata_command`、`test_metadata_manifest`、`test_metadata_records`
  - 日志：`tmp/test-logs/t005-build.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^(MetadataCommandTest|MetadataManifestTest)\\."`
  - 结果：`PASS`
  - 统计：`16/16` 通过
  - 日志：`tmp/test-logs/t005-ctest.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^MetadataRecordTypesTest\\."`
  - 结果：`PASS`
  - 统计：`4/4` 通过
  - 日志：`tmp/test-logs/t005-ctest-records.log`
- 本任务只运行了 metadata records 相关个别测试，未运行全量 CTest。

## 6. 未执行测试说明

- 未运行 `./test.sh --group all` 或全量 `ctest`；原因是 T005 只增加基础数据结构，不涉及节点装配、服务主路径或恢复语义。
- Windows configure/build/CTest 未执行；原因与 T004 相同，当前环境为 Linux，`windows` preset 依赖 `Visual Studio 17 2022` generator。
- 未执行 metadata state machine / metadata service / snapshot / recovery / catch-up 测试；这些超出 T005 范围。

## 7. 风险点

- 当前仓库仍以 V1 `MetadataRecord / Tombstone / IdempotencyEntry` 参与状态机与服务逻辑；本次新增的 `BucketRecord / ObjectRecord / ChunkRef / RequestRecord` 还未接入主路径。
- `RequestRecord` 目前只定义了结构骨架，尚未绑定真正的 request_id 幂等流程。
- `ObjectRecord` 与现有 `MetadataRecord` 暂时并存；后续 T006+ 需要明确迁移桥接策略，避免 V1/V2 语义分叉。
- 新结构本身未实现序列化函数；本次只确保字段形态简单、后续易序列化。
- KV removal status：`未开始删除`，当前 KV 路径保持原样。

## 8. 验收结果

- 已定义 `BucketRecord / ObjectRecord / ChunkRef / RequestRecord`。
- 已定义 `ObjectState: PENDING / COMMITTED / DELETED`。
- `ChunkRef` 只保存元数据引用，不保存真实数据。
- 新结构不依赖 `KVCommand / KvStateMachine / KvService`。
- 必要 CMake 更新已完成：新增 `test_metadata_records`。
- 相关 `AGENTS.md` 已同步更新，且未写入执行日志。
- 已完成 Linux 相关最小构建与个别测试验证。
- 未盲目运行全量 CTest。
- 未删除 KV。
- 未修改 `RaftNode` 默认状态机装配。
- 未进入 T006 或后续任务。
