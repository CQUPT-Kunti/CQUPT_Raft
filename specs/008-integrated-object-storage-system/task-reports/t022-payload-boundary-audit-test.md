# T022 Payload Boundary Audit Test

## 1. 修改文件

- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t022-payload-boundary-audit-test.md`

`tests/CMakeLists.txt` 未修改。该 target 占位已由 T006 接入，本次只补齐缺失测试源文件。

## 2. 新增测试覆盖的边界

本次在 `tests/integrated_object_storage_e2e_test.cpp` 新增了 3 个轻量 audit-style 测试，覆盖以下边界：

1. metadata/control-plane protobuf descriptor 边界
   - 校验 `CreateObjectRequest`、`CommitObjectRequest`、`ChunkRef`、`ObjectRecord`、`HeadObjectResponse`、`ListObjectsResponse` 不存在 `payload` 字段。
   - 校验上述 metadata proto 不包含 `bytes` 类型字段。
   - 对照校验 `storage::WriteChunkRequest`、`storage::ReadChunkResponse` 仍然保留 `payload`/`bytes`，证明真实 chunk bytes 仅属于 StorageNode data-plane。

2. metadata command serialization 边界
   - 校验 008 使用的 `CreateObject` / `CommitObject` V2 metadata command 序列化结果不包含 `record_payload=` 或 `payload` 关键字。
   - 校验序列化结果只包含 manifest facts，例如 `bucket`、`object_id`、`size`、`chunk_id`、`offset`、`checksum`、`replica_nodes`。

3. metadata snapshot 边界
   - 使用现有 `MetadataStateMachine` 做 `CreateBucket -> CreateObject -> CommitObject` 轻量 round-trip。
   - 校验快照文件中存在 manifest facts（如 `chunk_id`、`checksum`），但不存在 `record_payload=` 或 `payload` 关键字。
   - 校验恢复后的 `HeadObject` / `FindChunkRefs` 只还原 metadata facts，不引入真实 payload。

## 3. 是否发现 payload 进入 metadata/Raft 的风险

发现一个既有风险面，但本任务未修改其生产行为：

- 旧版 metadata 命令模型 `modules/raft/common/metadata_command.h` 中仍保留 `MetadataRecord::payload`，且 legacy create command 序列化路径会写出 `record_payload`。
- 该路径属于仓库既有 metadata 模型，不是 008 对象存储的 `CreateObject` / `CommitObject` manifest 路径。
- 本次 T022 新增测试明确把 008 边界锁定在 V2 object metadata command、metadata proto manifest 和 metadata snapshot 上，防止后续 008 实现把真实文件内容或 chunk bytes 回流到这些路径。

结论：

- 未发现当前 008 对象 manifest / chunk manifest 类型本身携带真实 payload 字段。
- 但仓库中存在 legacy `MetadataRecord::payload` 这一既有风险面，后续实现 008 上传流程时仍需避免误回退到 legacy payload 语义。

## 4. 是否保持测试轻量、平台中立、不过早实现 E2E 流程

是。

- 测试只做 descriptor、序列化和 snapshot 审计，不实现 upload/download 生产流程。
- 不启动真实多进程集群，不依赖固定 Linux-only 路径。
- 仅使用现有 metadata 类型和状态机做轻量 round-trip，便于后续持续作为防回归保护。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

- `common-risk-notes.md` 未修改。
- `risk-register.md` 未修改。
- 原因：本任务范围限定为 T022 测试补充；发现的 legacy `MetadataRecord::payload` 风险已在本报告中明确记录，但不在本任务内扩展文档面。

## 6. 验证命令和结果

执行命令：

```bash
git diff -- tests/integrated_object_storage_e2e_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t022-payload-boundary-audit-test.md
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target test_integrated_object_storage_e2e
ctest --test-dir build/linux -R 'IntegratedObjectStorageE2ETest\\.' --output-on-failure
```

结果：

- `git diff`：`tests/integrated_object_storage_e2e_test.cpp` 和任务报告仅包含 T022 相关新增；`tasks.md` 中除 T022 外还包含本次开始前已存在的 T010/T012/T015/T020 未提交状态变更，需要与本任务区分查看。
- `cmake --preset debug-ninja-low-parallel`：PASS，耗时约 3 秒。
- `cmake --build --preset debug-ninja-low-parallel --target test_integrated_object_storage_e2e`：PASS，耗时约 66 秒。
- `ctest --test-dir build/linux -R 'IntegratedObjectStorageE2ETest\\.' --output-on-failure`：PASS，3/3 通过，总耗时约 0.04 秒。

说明：

- `debug-ninja-low-parallel` preset 的实际 `binaryDir` 是 `build/linux`，不是 `build/linux/debug-ninja-low-parallel`，因此 `ctest` 路径按 `CMakePresets.json` 的实际配置调整。
- `gtest_discover_tests()` 注册出的测试名是 `IntegratedObjectStorageE2ETest.*`，因此 `ctest -R` 按真实用例名匹配，而不是按 target 名称匹配。
