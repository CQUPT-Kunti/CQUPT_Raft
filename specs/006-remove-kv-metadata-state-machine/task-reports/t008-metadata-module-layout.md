# T008 Metadata 模块文件结构拆分

## 1. T008 结论

- T008 已完成，范围仅限 metadata 头文件边界拆分、include 路径整理、AGENTS 同步和最小构建验证。
- 本次没有实现新的业务语义，没有实现序列化，没有接入 `RaftNode`，没有删除 KV。
- 当前 metadata V2 相关结构已从 `raft/common` 的临时堆积状态拆出到独立的 `raft/metadata` 模块。
- `raft/common/metadata_command.h` 仍保留为过渡入口，用于兼容现有 `metadata_command.cpp` 的 V1 序列化/校验实现和现有调用方。

## 2. 新增/移动/修改文件

- 新增 `modules/raft/metadata/metadata_records.h`
- 新增 `modules/raft/metadata/metadata_command_types.h`
- 新增 `modules/raft/metadata/metadata_query.h`
- 新增 `modules/raft/metadata/AGENTS.md`
- 修改 `modules/raft/common/metadata_command.h`
- 修改 `tests/metadata_records_test.cpp`
- 修改 `tests/metadata_command_types_test.cpp`
- 修改 `AGENTS.md`
- 修改 `modules/raft/common/AGENTS.md`
- 修改 `modules/raft/state_machine/AGENTS.md`
- 新增报告 `specs/006-remove-kv-metadata-state-machine/task-reports/t008-metadata-module-layout.md`

## 3. metadata 模块边界

- `metadata_records`
  - 文件：`modules/raft/metadata/metadata_records.h`
  - 职责：`BucketRecord`、`ObjectRecord`、`ChunkRef`、`RequestRecord`、`ObjectState`、`MetadataRequestType`
- `metadata_command_types`
  - 文件：`modules/raft/metadata/metadata_command_types.h`
  - 职责：`MetadataCommandType` 和 6 个写命令 payload
  - 包括：`CreateBucket`、`DeleteBucket`、`CreateObject`、`CommitObject`、`AbortObject`、`DeleteObject`
- `metadata_query`
  - 文件：`modules/raft/metadata/metadata_query.h`
  - 职责：`HeadObjectQuery`、`ListObjectsQuery`
- `metadata_manifest`
  - 当前未单独拆成新头文件
  - 现状：manifest 校验仍与现有 V1 `metadata_command.cpp` 的序列化/校验逻辑同处一处
  - 原因：T008 不越界进入 T007 的序列化重构
- `metadata_state_machine`
  - 本次不改实现
  - 后续位置仍在 `modules/raft/state_machine/metadata_state_machine.h/.cpp`
  - 已在 `modules/raft/state_machine/AGENTS.md` 中补齐说明

## 4. CMake 更新

- 本次没有修改根 `CMakeLists.txt`，也没有新增新的生产 target。
- `tests/CMakeLists.txt` 本次无需调整；沿用现有 metadata 单测 target 即可覆盖拆分后的 include 路径。
- 结论：target 图保持不变，拆分主要是头文件布局和 include 边界变化。

## 5. AGENTS.md 更新

- 根 `AGENTS.md`
  - 新增 `modules/raft/metadata` 到模块索引
  - 在 include 规则中新增 `raft/metadata/...`
  - 将 `modules/raft/state_machine` 描述更新为 `KV / metadata 状态机与状态机快照格式`
- `modules/raft/common/AGENTS.md`
  - 明确 `metadata_command.h` 是 metadata 过渡期入口头文件
  - 明确 metadata records / payload / query model 的正式边界在 `raft/metadata/`
- `modules/raft/state_machine/AGENTS.md`
  - 补齐 `metadata_state_machine.h/.cpp`
  - 补齐 metadata 相关测试入口与职责说明
- 新增 `modules/raft/metadata/AGENTS.md`
  - 明确 records / command payload / query model 的职责边界
- 所有 AGENTS 更新都只涉及结构说明，没有写执行日志。

## 6. 测试结果

- Linux configure：
  - 命令：`cmake --preset debug-ninja-low-parallel`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t008-configure.log`
- Linux build：
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target test_metadata_command test_metadata_records test_metadata_command_types test_metadata_manifest`
  - 结果：`PASS`
  - 覆盖：重新编译受头文件拆分影响的 `raft_core` 和 4 个 metadata 单测 target
  - 日志：`tmp/test-logs/t008-build.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^(MetadataCommandTest|MetadataRecordTypesTest|MetadataCommandTypesTest|MetadataManifestTest)\\."`
  - 结果：`PASS`
  - 统计：`26/26` 通过
  - 日志：`tmp/test-logs/t008-ctest.log`
- 本任务仅运行相关个别测试，未运行全量 CTest。

## 7. 未执行测试说明

- 未运行 `./test.sh --group all` 或全量 `ctest`；原因是 T008 只做头文件结构拆分，不涉及节点装配、服务入口、恢复语义或 proto。
- Windows configure/build/CTest 未执行；原因与 T004/T005/T006 相同，当前环境为 Linux，`windows` preset 依赖 `Visual Studio 17 2022` generator。
- 未运行 `MetadataStateMachine` / `MetadataService` / snapshot / recovery 测试；这些不属于 T008 验证范围。

## 8. 风险点

- 当前处于过渡态：`raft/common/metadata_command.h` 仍是 umbrella header，同时承载 V1 序列化边界和 V2 类型聚合。
- `metadata_manifest` 仍未独立拆成专门头文件；后续 T007 处理序列化时需要重新判断是否继续拆分。
- 生产代码中的现有调用方大多仍包含 `raft/common/metadata_command.h`，还没有全面切到 `raft/metadata/...` 直连 include。
- 本次结构拆分未改变 target 图，因此“模块物理边界更清晰”已完成，但“业务层完全脱离 common”还需要后续任务继续推进。
- KV removal status：`未开始删除`。

## 9. 验收结果

- metadata 相关文件结构已更清晰。
- `records / command payload / query` 边界已明确。
- 新增 `raft/metadata` 模块不依赖 KV。
- 本次无新的 CMake target 变更需求，现有 metadata 测试 target 可直接复用。
- 相关 `AGENTS.md` 已同步更新模块索引、文件位置和职责说明。
- metadata 相关测试通过。
- 未盲目运行全量 CTest。
- 未删除 KV。
- 未修改 `RaftNode` 默认状态机装配。
- 未实现 `MetadataStateMachine` apply。
- 未拆 proto。
- 未进入 T009 或后续任务。
