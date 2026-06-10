# T080 任务报告

## 1. 修改了哪些文件

- `modules/store/maintenance/garbage_collector.h`
- `modules/store/maintenance/garbage_collector.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_garbage_collector_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t080-orphan-staging-cleanup-hook.md`

未修改：

- `common-risk-notes.md`
- `risk-register.md`

说明：

- `tasks.md` 工作树中已存在其他任务状态变更；本任务只新增了 `T080` 的勾选，没有调整其他任务内容。

## 2. orphan/staging cleanup integration hook 做了什么

- 为 `GarbageCollector` 新增 `SubmitCleanupCandidates(const std::vector<CleanupCandidate>&)`。
- 这个 hook 把上游已经生成好的 cleanup candidates 统一收敛为：
  - `CleanupCandidate`
  - `CleanupCandidateToGarbageCollectorTask(...)`
  - `SubmitTask(...)`
  的标准接入链路。
- hook 返回逐 candidate 的诊断信息，包含：
  - cleanup source
  - object state
  - garbage collection reason
  - chunk identity / task_id
  - metadata boundary
  - submit code / status
  - queue depth
  - accepted / already_exists
- `AlreadyExists` 被保留为可诊断的幂等事实，便于后续 T081 把 failed upload session 发出的 cleanup candidates 接到同一个入口，而不会静默吞掉重复请求。

## 3. 如何保证不误删已 COMMITTED 对象的 live chunk

- 本次新增的 hook 不直接做删除，只做 candidate 到 GC task 的接入。
- 真正执行删除前，仍然必须经过现有 `metadata-driven safety checker`。
- 因此：
  - hook 不拥有 manifest authority
  - hook 不根据本地 chunk 状态判断对象是否 COMMITTED
  - hook 不会绕过 live-manifest protection
- 已 COMMITTED live chunk 是否允许删除，仍由 safety checker 基于 metadata boundary 和上游权威事实决定。

## 4. 如何保持 StorageNode cleanup 与 MetadataNode object visibility authority 的边界

- cleanup hook 只消费上游显式提供的 `CleanupCandidate` 和 `metadata_boundary`。
- 它不会调用 `MetadataStateMachine`、Raft、metadata service，也不会改变对象 `PENDING/COMMITTED/DELETED` 可见性。
- hook 只负责把“可清理候选”转成 GC task 并登记到 bounded queue；删除是否可执行仍要经过 safety checker，object visibility authority 仍留在 MetadataNode。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `CleanupCandidate` 模型仍然以 chunk identity 为中心；本次 hook 能统一接入 orphan / pending-timeout / failed-upload / abort / deleted cleanup candidate，但更细的 staging-only 实体如何由上游编码，仍取决于后续任务对 candidate emission 的补充。
- `AlreadyExists` 当前被视为可诊断幂等事实而不是整体失败；这符合“重复 cleanup 请求应幂等或可诊断”的要求，但如果后续调用方希望把重复请求视为硬错误，需要在调用侧再收紧策略。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 7. 验证命令和结果

执行的 diff 检查：

```bash
git diff -- modules/store/maintenance/garbage_collector.cpp modules/store/maintenance/garbage_collector.h modules/store/maintenance/module-notes.md tests/storage_garbage_collector_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t080-orphan-staging-cleanup-hook.md
```

结果：已检查，改动范围符合 T080。

执行的最小构建：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_storage_garbage_collector'
```

结果：PASS。

执行的最小相关测试：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'build/linux/safe/tests/test_storage_garbage_collector --gtest_filter=StorageGarbageCollectorTest.SubmitCleanupCandidatesAcceptsFailedUploadCandidateAndPreservesBoundary:StorageGarbageCollectorTest.SubmitCleanupCandidatesReportsDuplicateCandidateAsDiagnosableIdempotentFact'
```

结果：PASS，2 个 T080 相关单测通过。
