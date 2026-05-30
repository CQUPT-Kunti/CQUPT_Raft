# T036 Upload Manifest Durable Facts

## 修改文件

- `tests/support/storage_upload_test_utils.h`
- `tests/storage_upload_integration_test.cpp`
- `modules/store/upload/module-notes.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `InMemoryUploadMetadataClient` 增加最近一次 `CreateObjectRequest` / `CommitObjectRequest` 记录，便于测试直接断言 metadata commit 输入
- 在 `storage_upload_integration_test.cpp` 新增通过真实 `UploadCoordinator`、真实 `LocalDiskChunkStore` 和 metadata test adapter 的 T036 集成测试
- 固定 `CommitObjectRequest.chunks` 必须只写入 durable success 副本，并与本地读回的 chunk durable facts 一致
- 补充 `modules/store/upload/module-notes.md`，把 `upload_coordinator.cpp` 中的构造函数、虚析构和关键 helper 说明补齐

## 当前任务输入、输出和成功/失败语义

- 输入：
  - `UploadCoordinatorRequest`
  - 两个真实 `LocalDiskChunkStore`
  - 一个强制返回 `OVERLOADED` 的非 durable 第三副本
- 输出：
  - `CommitObjectRequest.chunks`
  - metadata 中最终保存的 `ChunkRef` manifest
- 成功语义：
  - `CommitObjectRequest.chunks` 的 `chunk_id`、`offset`、`size`、`checksum`、`replica_nodes` 与本地 durable 成功副本读回事实一致
  - 非 durable 失败副本不会进入 `replica_nodes`
- 失败语义：
  - 当前测试只固定 manifest 与 durable facts 的契约，不新增 `AbortObject`、GC 或 orphan cleanup

## 是否使用 tests/test_file/test_file.deb

- 是

## node-data 可视化目录路径和保留内容（如适用）

- 本任务未使用 `node-data/`
- 测试使用 `tests/support/store_test_utils.h` 的临时目录

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "upload_coordinator|storage_upload" --output-on-failure 2>&1 | tee tmp/007/t036-test.log`
  - PASS
  - 日志路径：`tmp/007/t036-test.log`

## Windows 验证判断

- 本任务只新增测试辅助可观察性和 Linux 当前集成断言
- 未新增 `T036-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T036

- 是

## 是否可以进入 T037

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- 当前闭环测试仍通过 `MetadataStateMachine` test adapter 观察 metadata 结果，不是“真实多 Raft 节点 + 真实 metadata gRPC client”的端到端测试
- T036 只固定 manifest durable facts 契约，没有解决 `AbortObject`、orphan chunk cleanup、pending object cleanup 或 restart recovery
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/upload/module-notes.md`
- 未修改 `AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补 `UploadMetadataClient::~UploadMetadataClient()`、`UploadChunkWriter::~UploadChunkWriter()`、`UploadCoordinator::UploadCoordinator(...)`
- `upload_coordinator.cpp` 中的匿名 namespace helper 和 `UploadCoordinator::UploadObject()` 流程说明保持完整

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T036 标记完成

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 更新：
  - T027 去掉“manifest durable facts 仍待补齐”的表述，改为记录 T036 已固定该契约，后续仍需处理 pending/orphan/cleanup/recovery 风险
