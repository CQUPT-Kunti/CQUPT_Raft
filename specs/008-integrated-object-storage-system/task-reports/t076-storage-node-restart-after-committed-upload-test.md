# T076 StorageNode restart after committed upload 测试报告

## 1. 修改了哪些文件

- `tests/integrated_object_storage_recovery_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t076-storage-node-restart-after-committed-upload-test.md`

未修改：

- `tests/support/integrated_cluster_test_utils.h`
- `tests/CMakeLists.txt`
- 生产代码
- `proto/`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T076 的 StorageNode restart after committed upload 测试做了什么

新增可运行测试：

- `IntegratedObjectStorageRecoveryTest.StorageNodeRestartAfterCommittedUploadKeepsManifestReadableChecksumsStableAndIdentityPinned`

测试步骤：

1. 在临时目录生成可控 payload，并写入 source 文件。
2. 为 StorageNode 同一 `data_dir` 创建/加载稳定 `node.identity`。
3. 使用 `LocalDiskChunkStore` 将对象拆成两个 chunk 写入同一 StorageNode data-plane。
4. 根据 `WriteChunk` 返回的 chunk metadata 构造 committed manifest。
5. 通过 `MetadataStateMachine` 执行 `CreateBucket -> CreateObject -> CommitObject`，把 committed manifest 固化为 metadata control-plane 权威事实。
6. 校验 committed manifest 可查询，并且 metadata command / metadata snapshot 不包含真实 payload。
7. 用新的 `ChunkIndex` 和新的 `LocalDiskChunkStore` 实例、但相同 `data_dir`，模拟 StorageNode restart。
8. 重启后严格按 committed manifest 的 chunk 列表读取 chunk，不绕过 manifest 直读本地文件。
9. 对每个 chunk 验证 checksum，并把读取结果重组为下载文件。
10. 对下载文件做最终 SHA-256 校验，并确认与 source 文件一致。

## 3. 是否验证 committed manifest、chunk checksum、final SHA-256 和 node.identity 稳定性

已验证。

- committed manifest：
  - `MetadataStateMachine::HeadObject(...)` 返回 `COMMITTED`
  - `MetadataStateMachine::FindChunkRefs(...)` 返回与 commit 时一致的 chunk manifest

- chunk checksum：
  - 重启后每个 `ReadChunk(...)` 都带 `expected_checksum + verify_checksum=true`
  - 断言 `read.actual_checksum.value == manifest checksum`
  - 断言 `read.metadata.checksum.value == manifest checksum`

- final SHA-256：
  - 将按 manifest 顺序读取到的 chunk payload 重组为下载文件
  - 对下载文件执行 SHA-256，要求与 source 文件完全一致

- `node.identity` 稳定性：
  - 首次通过 `LoadOrCreateNodeIdentity(...)` 创建 StorageNode identity
  - 重启前后使用同一 `data_dir` 重新加载 identity
  - 断言 `cluster_id / node_id / node_type / source / raft_id` 保持一致，且 StorageNode 不携带 `raft_id`

## 4. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

没有。

本任务新增的是 enabled 测试，不是 disabled/scaffold。

## 5. 是否发现不合理点 / 警告 / 风险

- `tests/CMakeLists.txt` 当前只定义了真实 gtest target `test_integrated_object_storage_recovery`，没有像 e2e/quorum 那样再包一层 `integrated_object_storage_recovery` 自定义 target。本任务未改 CMake，按约束直接使用现有 gtest target 做最小构建；若后续需要统一 target 名称，由 T084 处理。
- `tasks.md` 当前工作树里已存在与本任务无关的 `T077`、`T078`、`T079` 已勾选差异；本任务只额外将 `T076` 从 `[ ]` 改为 `[X]`。
- 本测试故意以 committed manifest 作为对象重组顺序权威，没有把 `LocalDiskChunkStore` 重建后返回的 metadata offset 当成恢复排序依据；这是为了保持 metadata/control-plane 与 StorageNode data-plane 边界清晰。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 7. 验证命令和结果

执行命令：

```bash
git diff -- tests/integrated_object_storage_recovery_test.cpp tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t076-storage-node-restart-after-committed-upload-test.md
git diff --check -- tests/integrated_object_storage_recovery_test.cpp
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_recovery'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "IntegratedObjectStorageRecoveryTest\\.StorageNodeRestartAfterCommittedUploadKeepsManifestReadableChecksumsStableAndIdentityPinned" --output-on-failure'
```

结果：

- `git diff --check -- tests/integrated_object_storage_recovery_test.cpp`：PASS
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_recovery`：PASS
- `ctest --test-dir build/linux/safe -R "IntegratedObjectStorageRecoveryTest\\.StorageNodeRestartAfterCommittedUploadKeepsManifestReadableChecksumsStableAndIdentityPinned" --output-on-failure`：PASS

说明：

- 本任务没有修改 `tests/CMakeLists.txt`，但由于 guarded gtest target 已存在，新增测试文件后可以直接构建 `test_integrated_object_storage_recovery`。
