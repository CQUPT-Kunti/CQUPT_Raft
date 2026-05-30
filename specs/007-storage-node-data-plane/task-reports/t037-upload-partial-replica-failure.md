# T037 Upload Partial Replica Failure

## 修改文件

- `modules/store/upload/upload_coordinator.h`
- `modules/store/upload/upload_coordinator.cpp`
- `modules/store/upload/module-notes.md`
- `tests/storage_upload_coordinator_test.cpp`
- `tests/storage_upload_integration_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `UploadCoordinatorResult` 增加 `cleanup_candidates`
- 在 `UploadCoordinator::UploadObject()` 中，当未达到 `minimum_successful_writes` 或 `CommitObject` 失败时，把已 durable 的 chunk facts 收集为 cleanup candidate
- 保持当前边界为“记录 cleanup candidate”，不实现真实 GC，不实现 `AbortObject`
- 扩展上传协调测试和上传集成测试，固定部分副本写失败后的不可见性与 cleanup candidate 语义

## 当前任务输入、输出和成功/失败语义

- 输入：
  - `UploadCoordinatorRequest`
  - `ReplicaPolicy`
  - placement 候选节点
  - `UploadMetadataClient`
  - `UploadChunkWriter`
- 输出：
  - `status`
  - `pending_object_possible`
  - `orphan_chunk_possible`
  - `committed_chunks`
  - `cleanup_candidates`
  - `chunk_executions`
- 成功语义：
  - 达到 `minimum_successful_writes` 后才允许进入 `committed_chunks`
  - `CommitObject` 成功后对象可见
  - 成功返回时 `cleanup_candidates` 为空
- 失败语义：
  - 未达到 `minimum_successful_writes` 时不调用 `CommitObject`
  - `HeadObject` / `ListObjects` 不可见
  - 已 durable success 的 chunk facts 进入 `cleanup_candidates`
  - 当前仍只记录 cleanup candidate，不自动执行 GC 或 `AbortObject`

## cleanup candidate / AbortObject 当前边界

- 当前支持：
  - 在 `UploadCoordinatorResult.cleanup_candidates` 中记录需要后续清理的 durable chunk facts
- 当前不支持：
  - `AbortObject`
  - background cleanup / GC
  - restart 后 cleanup candidate 自动恢复执行

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "upload_coordinator|storage_upload" --output-on-failure 2>&1 | tee tmp/007/t037-test.log`
  - PASS
  - 日志路径：`tmp/007/t037-test.log`

## Windows 验证判断

- 本任务只新增 upload 失败路径结果表达和平台无关测试
- 未新增 `T037-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T037

- 是

## 是否可以进入 T038

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 `cleanup_candidates` 只是结果层事实，不会触发真实 GC、`AbortObject` 或重启后自动恢复处理
- 当前闭环测试仍通过 `MetadataStateMachine` test adapter 观察 metadata，不是“真实多 Raft 节点 + 真实 metadata gRPC client”的端到端测试
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/upload/module-notes.md`
- 未修改 `AGENTS.md`

## module-notes.md 是否补充了 .cpp 关键函数 / helper

- 是
- 已补 `BuildDurableChunkFacts(...)`
- 已补 `AppendCleanupCandidate(...)`
- 已补 `AppendCleanupCandidates(...)`
- 已补 `SortCleanupCandidates(...)`
- 并更新了 `UploadCoordinator::UploadObject()` 失败路径与 `cleanup_candidates` 语义

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：
  - 将 T037 标记完成
  - 将 T037 的实际修改路径纠偏到 `modules/store/upload/*`、`tests/storage_upload_coordinator_test.cpp` 和 `tests/storage_upload_integration_test.cpp`

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 更新：
  - T027 改为记录 T037 已固定 cleanup candidate 边界，但真实 cleanup / abort / recovery 协同仍未完成
