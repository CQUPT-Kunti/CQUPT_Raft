# T079 任务报告

## 1. 修改了哪些文件

- `tests/integrated_object_storage_recovery_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t079-no-healthy-storage-capacity-failure-test.md`

未修改：

- `tests/support/integrated_cluster_test_utils.h`
- `tests/CMakeLists.txt`
- `common-risk-notes.md`
- `risk-register.md`

说明：

- `tasks.md` 工作树中已存在其他任务状态变更；本任务只新增了 `T079` 的勾选，没有调整其他任务内容。

## 2. T079 的 no healthy StorageNode capacity failure 测试做了什么

- 新增 `IntegratedObjectStorageRecoveryTest.NoHealthyOrCapacitySufficientStorageFailsUploadAndKeepsObjectInvisible`。
- 该测试通过现有 `UploadCoordinator + InMemoryUploadMetadataClient + LocalStoreUploadChunkWriter` 组合，构造一个同时包含：
  - `Unavailable` StorageNode
  - `ReadOnly` StorageNode
  - `Healthy` 但容量不足的 StorageNode
  的 upload 场景。
- 触发 upload flow 后，断言：
  - placement 明确失败
  - 不产生可执行副本集合
  - 不发生 chunk 写入
  - 不尝试 `CommitObject`
  - 返回可诊断的 placement/capacity 失败信息

## 3. 是否验证无健康/容量不足时 WritePlan 或 upload flow 明确失败

已验证。

- `UploadCoordinatorResult.status == kNodeUnavailable`
- `error_detail` 包含 `PlacementManager failed for chunk` 和 `eligible nodes were insufficient`
- `chunk_execution.placement_decision.replica_nodes.empty() == true`
- exclusion reasons 明确覆盖：
  - `node health is not writable: Unavailable`
  - `node health is not writable: ReadOnly`
  - `node capacity is insufficient for requested chunk`

当前 upload 路径没有单独暴露 `WritePlan` 对象，因此本测试用“placement 决策没有任何 replica nodes 且 commit 不会发生”来锁定“不可执行 WritePlan / upload flow 明确失败”的边界。

## 4. 是否验证部分对象不可见

已验证。

- `HeadObject` 返回 `NotFound`
- `ListObjects` 为空
- `FindObject(...)` 仍可观察到内部 `PENDING` 记录
- `stored_object->chunks.empty() == true`
- `FindChunkRefs(...)` 不存在

这说明失败后没有进入 `COMMITTED` 可见路径，也没有生成可下载 manifest。

## 5. 是否有 disabled/scaffold 测试

没有新增 disabled/scaffold 测试。

当前现有 upload coordinator 路径已经足够支撑 T079 的最小可运行测试，因此直接落了可执行用例，没有为了后续任务提前写空骨架。

## 6. 是否发现不合理点 / 警告 / 风险

- 用户给出的示例构建 target 名是 `integrated_object_storage_recovery`，但当前 `tests/CMakeLists.txt` 只显式声明了 `test_integrated_object_storage_recovery`，没有像 quorum target 那样补 alias custom target。
- 当前 capacity failure 路径仍然会先执行 `CreateObject`，因此 `pending_object_possible == true`。本测试锁定的是“失败后不可见、不可 commit、无 chunk manifest”，不是“完全不产生内部 PENDING 记录”。如果后续产品要求 placement 失败前连 `CreateObject` 都不应发生，需要在后续实现任务中进一步收紧。

## 7. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 8. 验证命令和结果

执行的 diff 检查：

```bash
git diff -- tests/integrated_object_storage_recovery_test.cpp tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t079-no-healthy-storage-capacity-failure-test.md
```

结果：已检查，改动范围符合 T079。

按用户示例 target 尝试最小构建：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_recovery'
```

结果：需要按仓库当前实际 target 名调整；当前显式存在的是 `test_integrated_object_storage_recovery`。

实际执行的最小构建：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_recovery'
```

结果：PASS。

实际执行的最小测试命令建议：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'build/linux/safe/tests/test_integrated_object_storage_recovery --gtest_filter=IntegratedObjectStorageRecoveryTest.NoHealthyOrCapacitySufficientStorageFailsUploadAndKeepsObjectInvisible'
```

结果：PASS，单个 T079 用例通过。
