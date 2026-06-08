# T050 任务报告：3-voter quorum insufficiency object commit 测试

## 1. 修改了哪些文件

- `tests/integrated_object_storage_quorum_test.cpp`
- `tests/CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t050-3voter-quorum-insufficiency-test.md`

未修改：

- `tests/support/integrated_cluster_test_utils.h`
- `specs/008-integrated-object-storage-system/common-risk-notes.md`
- `specs/008-integrated-object-storage-system/risk-register.md`

## 2. T050 的 3-voter quorum insufficiency 测试做了什么

新增 `IntegratedObjectStorageQuorumTest.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive`，测试步骤如下：

1. 启动 3 个 Raft voter 的 MetadataNode 测试集群，等待单 leader 选举完成。
2. 在 quorum 完整时先提交 `CreateBucket`，再提交 `CreateObject`，确保对象已经以 `PENDING` 状态复制到 3 个 voter。
3. 记录 leader 上的 `request_count` 和 `last_applied_index` 基线。
4. 停止另外 2 个 voter，只保留 1 个 live 节点。
5. 让剩余节点尝试执行 `CommitObject`。
6. 断言提交不能成功，并且失败形态必须落在 quorum 不足允许的诊断范围内：
   - `Timeout`
   - `ReplicationFailed`
   - `CommitFailed`
   - `NotLeader`
7. 断言对象没有被错误标记为 `COMMITTED`：
   - `HeadObject` 仍然不可见
   - 内部对象状态仍为 `PENDING`
   - `FindIndexedObjectId` / `FindChunkRefs` 仍不存在
   - `request_count` 与 `last_applied_index` 不前进

这个测试锁定了 3-voter committed membership 下 quorum 仍然是 2，而不是随着 live 节点数降为 1。

## 3. 是否有 disabled/scaffold 测试

没有。

本任务新增的是可编译、可执行的真实 quorum safety 测试，不是 disabled/scaffold 占位用例。

## 4. 是否发现不合理点 / 警告 / 风险

- 发现现有 `CMakePresets.json` 中 `debug-tests` preset 指向 `build/linux`，而本任务按要求使用的 `debug-ninja-safe` 构建目录是 `build/linux/safe`。因此直接执行：
  - `ctest --preset debug-tests -R "<T050测试名>" --output-on-failure`
  在当前仓库状态下会出现 `No tests were found!!!`，并不是 T050 自身失败，而是 preset 与 safe 构建目录不一致。
- `tests/CMakeLists.txt` 原先只为 `integrated_object_storage_e2e` 暴露了同名 custom target，没有为 quorum target 暴露 `integrated_object_storage_quorum`。本任务做了最小补齐，使用户指定的 `cmake --build --preset debug-ninja-safe --target integrated_object_storage_quorum` 可直接使用。
- 本窗口尝试用安全构建目录执行单测时，构建锁被占用，未等待、未重试，符合任务要求。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

原因：T050 仅新增 quorum safety 测试和最小构建接入，没有引入新的生产风险登记需求。

## 6. 验证命令和结果

### 6.1 diff 检查

命令：

```bash
git diff -- tests/integrated_object_storage_quorum_test.cpp tests/CMakeLists.txt tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t050-3voter-quorum-insufficiency-test.md
```

结果：

- PASS
- 变更范围符合 T050 边界

### 6.2 定向 configure + build

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_quorum' || echo "build lock busy, skip build in this window"
```

结果：

- PASS
- `integrated_object_storage_quorum` 成功完成 configure/build

### 6.3 按用户给定 ctest preset 的一次尝试

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "^IntegratedObjectStorageQuorumTest\\.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive$" --output-on-failure' || echo "build/test lock busy, skip test in this window"
```

结果：

- 未发现测试
- 摘要：`No tests were found!!!`
- 说明：`debug-tests` 指向 `build/linux`，而本任务实际构建目录是 `build/linux/safe`
- 本地日志：`tmp/test-logs/t050-ctest.log`

### 6.4 针对 safe 构建目录的单测尝试

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "^IntegratedObjectStorageQuorumTest\\.ThreeVoterCommittedMembershipDoesNotShrinkQuorumWhenOnlyOneNodeRemainsLive$" --output-on-failure' || echo "build/test lock busy, skip test in this window"
```

结果：

- 构建锁被占用，本窗口未执行 build/test，待统一验证
- 本地日志：`tmp/test-logs/t050-ctest-safe.log`

## 7. 结论

- T050 代码实现已完成。
- 定向 build 已通过。
- 受构建锁限制，本窗口未完成 T050 单测执行，因此当前结论是“实现完成，验证待统一补跑”，不能在本报告中宣称测试已最终通过。
