# T007 MetadataCommand 序列化 / 反序列化

## 1. T007 结论

- T007 已完成，范围仅限 `MetadataCommand` 的 `META1` 编码、解析、校验与 fingerprint 扩展。
- 本次在 T008 的文件布局基础上实现，没有回退 `raft/metadata` 模块结构。
- 现有 V1 `Create/Commit/Delete` 路径保持可编译、可解析。
- 新增 V2 写命令的序列化 / 反序列化支持：
  - `CreateBucket`
  - `DeleteBucket`
  - `CreateObject`
  - `CommitObject`
  - `AbortObject`
  - `DeleteObject`
- `HeadObject` / `ListObjects` 仍然只是查询模型，没有进入普通 Raft 写日志命令编码。

## 2. 新增/修改文件

- 修改 `modules/raft/common/metadata_command.cpp`
- 修改 `tests/metadata_command_test.cpp`
- 新增报告 `specs/006-remove-kv-metadata-state-machine/task-reports/t007-metadata-command-serialization.md`

## 3. 序列化覆盖范围

- 保持 `META1` envelope 不变，继续使用逐字段 `key=value` 文本格式。
- 顶层幂等字段 `request_id` 保留在 `MetadataCommand` 顶层。
- V2 写命令通过 `command_type` 字段区分类型。
- `CreateBucket` / `DeleteBucket`
  - 支持 bucket 相关 payload 编解码
- `CreateObject`
  - 支持 `ObjectRecord` 基础字段编解码
- `CommitObject`
  - 支持 bucket/object identity、version、size、etag、`ChunkRef` 列表、可选 `commit_time`
- `AbortObject` / `DeleteObject`
  - 支持 object identity 与版本相关 payload 编解码
- 可选 `request_context` 支持编解码，为后续幂等 / apply 上下文预留
- 旧 V1 `MetadataRecord` create path 仍通过原有 `record_*` 字段工作

## 4. 非法输入拒绝范围

- 未知 `command_type` 被拒绝
- 截断的 commit chunk payload 被拒绝
- 损坏的 chunk list（例如非数字 offset）被拒绝
- `request_context.command_type` 与 `MetadataCommand.command_type` 不一致时被拒绝
- 旧 V1 create record 的 payload 上限、object_key 缺失、request_id 缺失等原有拒绝逻辑保持有效

## 5. 关键实现点

- `MetadataOperation::kUnknown` 现在允许作为 V2 命令的 legacy 占位值被解析，而不是在 parse 阶段提前失败。
- `MetadataCommand` 的验证逻辑改成：
  - `command_type != kUnknown` 时走 V2 命令校验
  - 否则继续走旧 V1 `operation` 校验
- `CommitObject` 的 `ChunkRef` 使用显式 `target_chunk_count + target_chunk_{i}_*` 字段编码，便于检测截断和损坏输入
- `ComputeMetadataCommandFingerprint()` 已扩展到 V2 命令，确保同 `request_id` 不同 payload 能区分

## 6. CMake / AGENTS 更新情况

- 本次未修改 `CMakeLists.txt`、`tests/CMakeLists.txt`
- 本次未新增或移动 metadata 模块文件
- 本次未更新 `AGENTS.md`
- 原因：T007 只扩展现有实现与现有测试，不改变模块边界和测试入口结构

## 7. 测试结果

- Linux configure：
  - 命令：`cmake --preset debug-ninja-low-parallel`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t007-configure.log`
- Linux build：
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target test_metadata_command test_metadata_command_types test_metadata_records`
  - 结果：`PASS`
  - 覆盖：重编 `metadata_command.cpp`、`test_metadata_command`、`test_metadata_command_types`、`test_metadata_records`
  - 日志：`tmp/test-logs/t007-build.log`
- Linux CTest：
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^(MetadataCommandTest|MetadataCommandTypesTest|MetadataRecordTypesTest)\\."`
  - 结果：`PASS`
  - 统计：`29/29` 通过
  - 说明：包含新加的 10 个 `MetadataCommandTest` V2 / 非法输入用例
  - 日志：`tmp/test-logs/t007-ctest.log`
- 本任务只运行了 T007 相关个别测试，未运行全量 CTest。

## 8. 未执行测试说明

- 未运行 `MetadataManifestTest`；原因是 T007 重点是命令编解码，不是 manifest 规则本身
- 未运行 `./test.sh --group all` 或全量 `ctest`；原因是本任务不涉及节点装配、状态机 apply、snapshot、service、proto
- Windows configure/build/CTest 未执行；原因与前序任务一致，当前环境为 Linux，`windows` preset 依赖 `Visual Studio 17 2022` generator

## 9. 风险点

- 当前仍是过渡态：`metadata_command.cpp` 同时承载 V1 和 V2 编解码逻辑，后续需要持续收敛
- `request_context` 已能编解码，但尚未接入真实 `MetadataStateMachine` apply 流程
- `metadata_manifest` 仍与旧 V1 create record 校验逻辑耦合，后续如要进一步拆分需在 T009+ 处理
- 本次没有接入 `RaftNode`，因此尚未验证日志落盘后的 end-to-end 兼容性
- KV removal status：`未开始删除`

## 10. 验收结果

- `MetadataCommand` 已支持 V1 编码格式下的 V2 写命令序列化 / 反序列化
- 已覆盖 6 类写命令
- `request_id` 顶层幂等字段已保留
- `CommitObject` 已完整支持 `ChunkRef` 列表
- 非法输入、未知类型、截断 payload、损坏 chunk list 已有拒绝测试
- `HeadObject` / `ListObjects` 仍未作为普通写命令编码
- 未删除 KV
- 未接入 `RaftNode`
- 未实现 `MetadataStateMachine` apply / Snapshot / MetadataService / proto 拆分
- 未进入 T009 或后续任务
