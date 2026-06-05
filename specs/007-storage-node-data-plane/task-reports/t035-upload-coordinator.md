# T035 Upload Coordinator

## 修改文件

- `modules/store/upload/upload_coordinator.h`
- `modules/store/upload/upload_coordinator.cpp`
- `modules/store/upload/module-notes.md`
- `modules/store/upload/AGENTS.md`
- `modules/store/AGENTS.md`
- `tests/support/storage_upload_test_utils.h`
- `tests/storage_upload_coordinator_test.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 新增 `modules/store/upload` 子模块，落地最小 `UploadCoordinator`
- `UploadCoordinator` 负责串联：
  - `CreateObject`
  - `PlacementManager`
  - `UploadChunkWriter::WriteChunkToNode`
  - `CommitObject`
- coordinator 只依赖抽象 `UploadMetadataClient` 和 `UploadChunkWriter`
- 新增测试辅助 `tests/support/storage_upload_test_utils.h`
  - 提供 `MetadataStateMachine` 适配的内存 metadata client
  - 提供基于 `LocalDiskChunkStore` 的 chunk writer test adapter
  - 提供 `tests/test_file/test_file.deb` 真实二进制 payload 加载
- 新增 `storage_upload_coordinator` 单测，覆盖成功、写失败、commit 失败、placement 失败

## upload coordinator/helper 输入、输出、成功/失败语义

- 输入：
  - `request_id`
  - `bucket`
  - `object_key`
  - `object_id`
  - `version`
  - `etag`
  - `chunks[]`
    - `chunk_index`
    - `offset`
    - `payload`
    - `expected_size`
    - `expected_checksum`
  - `ReplicaPolicy`
  - `candidates`
  - `excluded_nodes`
  - `StorageTaskContext`
  - `client_time_unix_ms`
- 输出：
  - `status`
  - `error_detail`
  - `create_succeeded`
  - `committed`
  - `pending_object_possible`
  - `orphan_chunk_possible`
  - `committed_chunks`
  - `chunk_executions`
    - `placement_decision`
    - `replica_results`
    - `durable_success_count`
    - `commit_eligible`
- 成功语义：
  - 先 `CreateObject`
  - 每个 chunk 先 placement，再对选中的节点逐个 `WriteChunk`
  - 每个 chunk 至少达到 `minimum_successful_writes` 个 durable success 后，才允许进入 manifest
  - 全部 chunk 满足条件后才 `CommitObject`
  - `CommitObject` 成功后 `HeadObject/ListObjects` 可见
- 失败语义：
  - placement 失败：不写 chunk，不 commit；`pending_object_possible=true`
  - write 不足以达到 `minimum_successful_writes`：不 commit；如果已有 durable chunk，则 `orphan_chunk_possible=true`
  - commit 失败：返回明确错误；durable chunk 仍可能存在，因此 `orphan_chunk_possible=true`
  - 当前不自动 `AbortObject`，不自动 GC，不伪装失败已自动收口

## 是否使用 tests/test_file/test_file.deb

- 是

## node-data 可视化目录路径和保留内容（如适用）

- 本任务未使用 `node-data/`
- 测试使用 `tests/support/store_test_utils.h` 的临时目录
- 未保留新的可视化 data-plane 目录

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "upload_coordinator|storage_upload" --output-on-failure 2>&1 | tee tmp/007/t035-upload-coordinator-test.log`
  - PASS
  - 日志路径：`tmp/007/t035-upload-coordinator-test.log`

## Windows 验证判断

- 本任务只新增 coordinator/helper、抽象适配和 Linux 当前测试
- 未新增 `T035-WIN`
- 当前环境没有 Windows 编译/测试能力，不伪造 Windows PASS

## 是否通过 T035

- 是

## 是否可以进入 T036

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- 当前 `UploadCoordinator` 仍是最小 helper，不实现 `AbortObject`、GC、retry scheduler 或 restart recovery
- `CommitObject` 失败后 durable chunk 仍可能残留，当前只通过结果和公共风险说明显式暴露 orphan chunk 风险
- 当前测试用 `MetadataStateMachine` 适配器和本地 chunk writer test adapter 串联流程，还不是生产 metadata client / StorageNodeClient 编排

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/upload/module-notes.md`
- 更新了 `modules/store/upload/AGENTS.md`
- 更新了 `modules/store/AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
- 原因：将 T035 标记完成，并把实际落地路径纠偏为 `modules/store/upload/*` 与 `tests/storage_upload_coordinator_test.cpp`

## common-risk-notes.md 读取结果

- 已读取
- T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 更新：
  - T027 从“仍缺 upload coordinator”调整为“已补最小 UploadCoordinator，但 pending object / orphan chunk cleanup 仍未解决”
