# T077 uncommitted chunk cleanup 测试报告

## 1. 修改了哪些文件

- `tests/integrated_object_storage_recovery_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t077-uncommitted-chunk-cleanup-test.md`

未修改：

- `tests/support/integrated_cluster_test_utils.h`
- `tests/CMakeLists.txt`
- 生产代码
- `proto/`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. T077 的 uncommitted chunk cleanup 测试做了什么

- 新增 `IntegratedObjectStorageRecoveryTest.T077UncommittedWrittenChunkRemainsInvisibleAndLeavesCleanupScaffold`
  - 用 `MetadataStateMachine` 创建 bucket 和 `PENDING` 对象。
  - 用真实 `LocalDiskChunkStore` 写入一个 durable live chunk。
  - 故意不执行 `CommitObject`。
  - 断言普通 metadata 可见路径仍然不可见：
    - `HeadObject` 返回 `kNotFound`
    - `ListObjects` 不返回该对象
    - `FindChunkRefs` 不存在 committed manifest
  - 同时断言 StorageNode 上的 chunk 确实存在且可诊断：
    - `StatChunk` 返回 `ChunkState::kLive`
    - `ReadChunk` 能直接按 chunk_id 读取原 payload
    - `ListChunks` 能看到该 live chunk
  - 最后把“live chunk 存在 + metadata 无 committed refs”显式锁定为 cleanup scaffold 前置条件，供后续 T080/T081 接入 recovery/GC hook 时消费。

## 3. 是否验证未提交对象不可见、staging/orphan cleanup candidate 或后续 hook 边界

- 已验证未提交对象不可见：
  - `HeadObject` 隐藏
  - `ListObjects` 隐藏
  - 普通下载所依赖的 committed manifest chunk refs 不存在
- 已验证 cleanup scaffold 前置条件：
  - chunk 已真实写入 StorageNode 且处于 `LIVE`
  - metadata 侧没有 committed manifest 引用它
- 当前没有验证真实生产 cleanup hook 的删除行为；该部分留给后续 T080/T081。

## 4. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

- 有。
- 新增 `IntegratedObjectStorageRecoveryTest.DISABLED_T077CleanupHookScaffoldRequiresT080T081`
- 原因：
  - 当前任务只写 recovery/cleanup 测试，不实现生产 cleanup / GC hook。
  - `T080` 才接入 orphan/staging cleanup integration hook。
  - `T081` 才从 failed upload sessions 发出 cleanup candidates。
- 启用条件：
  - recovery 路径具备真实 cleanup candidate emission 和 hook 消费后，再把该 scaffold 改为可执行验证。

## 5. 是否发现不合理点 / 警告 / 风险

- `tests/CMakeLists.txt` 已经存在 `test_integrated_object_storage_recovery` target 的 guarded 接线，但此前源文件不存在；本次只补测试文件，没有修改 CMake。
- 当前 active 用例主要锁定 metadata visibility 和 cleanup scaffold 前置条件，不覆盖“重启后自动清理 orphan/staging chunk”的真正 recovery hook 行为；那是后续 T080/T081 的职责。
- 当前普通下载路径没有直接在该测试里跑端到端 client，而是通过 “no committed manifest refs” 间接证明 download 不应被元数据层放行，这与当前阶段的系统边界一致。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md；如未修改，明确说明未修改

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 7. 验证命令和结果；如果 target 尚未接入或构建锁被占用，也要说明

执行命令：

```bash
git diff -- tests/integrated_object_storage_recovery_test.cpp tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t077-uncommitted-chunk-cleanup-test.md
git diff --check -- tests/integrated_object_storage_recovery_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t077-uncommitted-chunk-cleanup-test.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_recovery'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/tests/test_integrated_object_storage_recovery --gtest_filter=IntegratedObjectStorageRecoveryTest.T077UncommittedWrittenChunkRemainsInvisibleAndLeavesCleanupScaffold --gtest_brief=1'
```

结果：

- `git diff`：PASS。`tasks.md` 在本任务相关区段只新增 `T077` 的勾选；测试源码和任务报告已纳入 diff。
- `git diff --check`：PASS。未发现空白错误。
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_recovery`：PASS。
- `./build/linux/safe/tests/test_integrated_object_storage_recovery --gtest_filter=IntegratedObjectStorageRecoveryTest.T077UncommittedWrittenChunkRemainsInvisibleAndLeavesCleanupScaffold --gtest_brief=1`：PASS，`1 test from 1 test suite ran`。
- 说明：第一次并行尝试时，定向单测因为构建锁被同窗口的 build 占用而按约束立即跳过；构建完成后已单独重跑并通过。
