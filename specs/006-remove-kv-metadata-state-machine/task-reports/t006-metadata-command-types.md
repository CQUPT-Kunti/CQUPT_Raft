# T006 MetadataCommand 类型定义

## 1. T006 结论

- T006 已完成，范围仅限 metadata command types 与 query model 类型骨架。
- 本次没有实现序列化、没有接入 `RaftNode`、没有改默认状态机装配、没有删除 KV。
- 在现有 `modules/raft/common/metadata_command.h` 上扩展了 V2 命令类型，保持旧 V1 `MetadataOperation` / `SerializeMetadataCommand()` 路径继续可编译。
- `HeadObject` / `ListObjects` 以只读查询模型定义，没有被放进普通 Raft 写日志命令集合。

## 2. 新增/修改文件

- 修改 `modules/raft/common/metadata_command.h`
- 新增 `tests/metadata_command_types_test.cpp`
- 修改 `tests/CMakeLists.txt`
- 修改 `modules/raft/common/AGENTS.md`
- 新增报告 `specs/006-remove-kv-metadata-state-machine/task-reports/t006-metadata-command-types.md`

## 3. 命令类型清单

- 新增 `MetadataCommandType`
  - `kCreateBucket`
  - `kDeleteBucket`
  - `kCreateObject`
  - `kCommitObject`
  - `kAbortObject`
  - `kDeleteObject`
- 保留 `request_id` 作为 `MetadataCommand` 顶层幂等字段。
- 在 `MetadataCommand` 中新增了 V2 payload 槽位：
  - `create_bucket`
  - `delete_bucket`
  - `create_object`
  - `commit_object`
  - `abort_object`
  - `delete_object`
- 新增 helper：
  - `IsWriteCommand()`
  - `IsCreateBucketCommand()`
  - `IsDeleteBucketCommand()`
  - `IsCreateObjectCommand()`
  - `IsCommitObjectCommand()`
  - `IsAbortObjectCommand()`
  - `IsDeleteObjectCommand()`
  - `CarriesChunkRefs()`

## 4. 查询模型清单

- 新增 `HeadObjectQuery`
  - 字段：`bucket`、`object_key`、`object_id`、`version`
- 新增 `ListObjectsQuery`
  - 字段：`bucket`、`prefix`、`limit`、`continuation_token`、`include_deleted`
- 这两个类型只表达读取模型，不是普通写命令，不进入 `MetadataCommandType`。

## 5. 与 T005 基础 records 的关系

- `CreateBucketCommandPayload` 基于 `BucketRecord`
- `CreateObjectCommandPayload` 基于 `ObjectRecord`
- `CommitObjectCommandPayload` 显式携带 `ChunkRef` 列表
- `MetadataCommand::request_context` 预留 `RequestRecord`，为后续幂等与 apply 上下文做准备
- `ObjectRecord` 继续使用 `ObjectState: PENDING / COMMITTED / DELETED`
- 新定义不依赖 `KVCommand`、`KvStateMachine`、`KvService`

## 6. CMake / AGENTS.md 更新情况

- `tests/CMakeLists.txt` 新增 `test_metadata_command_types` target。
- 未修改根 `CMakeLists.txt` 的业务 target；`raft_core`、`raft_demo`、`raft_kv_client`、`raft_metadata_client` 保持原样。
- `modules/raft/common/AGENTS.md` 已同步补充：
  - metadata command types / query skeleton 职责说明
  - `tests/metadata_command_types_test.cpp` 测试入口
- 未向 `AGENTS.md` 写入执行日志。

## 7. 测试结果

- Linux configure：
  - 命令：`cmake --preset debug-ninja-low-parallel`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t006-configure.log`
- Linux build：
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target test_metadata_command test_metadata_manifest test_metadata_records test_metadata_command_types`
  - 结果：`PASS`
  - 覆盖：重新编译受 `metadata_command.h` 影响的 `raft_core` 与 4 个 metadata 单测 target
  - 日志：`tmp/test-logs/t006-build.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^(MetadataCommandTest|MetadataManifestTest|MetadataRecordTypesTest)\\."`
  - 结果：`PASS`
  - 统计：`20/20` 通过
  - 日志：`tmp/test-logs/t006-ctest.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^MetadataCommandTypesTest\\."`
  - 结果：`PASS`
  - 统计：`6/6` 通过
  - 日志：`tmp/test-logs/t006-ctest-command-types.log`
- 本任务只运行了 T006 相关个别测试，未运行全量 CTest。

## 8. 风险点

- 当前 `metadata_command.cpp` 的序列化 / 解析 / 校验逻辑仍是 V1 `MetadataOperation` 模型，尚未理解新 `MetadataCommandType` payload。
- `MetadataCommand` 目前同时承载 V1 与 V2 字段，是过渡态设计；后续 T007 需要明确序列化边界，避免混用。
- `HeadObjectQuery` / `ListObjectsQuery` 仅为类型骨架，尚未与状态机查询接口统一命名或统一结果模型。
- `request_context` 只是预留位，尚未进入真实幂等流程。
- KV removal status：`未开始删除`。

## 9. 验收结果

- 已定义 `MetadataCommandType`。
- 已为 `MetadataCommand` 增加 `CreateBucket`、`DeleteBucket`、`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject` 的类型与 payload 槽位。
- `CommitObject` 已能携带 `ChunkRef` 列表。
- 已定义 `HeadObject` / `ListObjects` 查询模型，且未将其作为普通 Raft 写命令。
- 新定义基于 T005 的 `BucketRecord / ObjectRecord / ChunkRef / RequestRecord / ObjectState`。
- 已完成必要 CMake/AGENTS 同步。
- 已完成 Linux 最小构建与相关个别测试验证。
- 未运行全量 CTest。
- 未删除 KV，未接入 `RaftNode`，未进入 T007。
