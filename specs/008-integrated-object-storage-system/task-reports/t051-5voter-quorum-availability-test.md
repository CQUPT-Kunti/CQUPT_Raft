# T051 任务报告：5-voter quorum calculation and commit availability 测试

## 1. 修改了哪些文件

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t051-5voter-quorum-availability-test.md`

本任务未新增修改：

- `tests/CMakeLists.txt`
- `tests/support/integrated_cluster_test_utils.h`
- `specs/008-integrated-object-storage-system/common-risk-notes.md`
- `specs/008-integrated-object-storage-system/risk-register.md`

## 2. T051 的 5-voter quorum calculation / commit availability 测试做了什么

在 `tests/integrated_object_storage_quorum_test.cpp` 中新增：

- `IntegratedObjectStorageQuorumTest.FiveVoterCommittedMembershipKeepsQuorumThreeAndAllowsCommitWithThreeReachableVoters`

测试覆盖内容：

1. 启动 5 个 Raft voter 的 MetadataNode 测试集群，等待单 leader 形成。
2. 先在 5-voter 完整 membership 下提交 `CreateBucket`，建立后续对象写入前置条件。
3. 停掉 2 个 voter，其中包含原 leader 和另一个 voter，制造“仅剩 3 个 reachable voter”的边界。
4. 等待剩余 3 个 voter 重新完成 leader election，证明 5-voter membership 下 quorum=3 时仍有选主可用性。
5. 在这 3 个 reachable voter 上继续执行：
   - `CreateObject`
   - `CommitObject`
6. 断言：
   - 3 个可达 voter 可以合法完成 `CreateObject`
   - 3 个可达 voter 可以合法完成 `CommitObject`
   - 对象最终在存活节点上一致变为 `COMMITTED`
   - 可见性、索引映射、chunk refs、`last_applied_index` 都达到已提交状态

这个测试锁定的是：

- committed membership 为 5 个 voter 时，quorum 必须是 3
- 即使 2 个 voter 不可用，只要剩余 3 个 voter 可达，系统仍然必须保持 Raft 规则下的提交可用性
- 可用性来自 surviving majority，而不是来自按 live node 数缩小 quorum

## 3. 是否有 disabled/scaffold 测试

没有。

本任务新增的是可编译的真实 5-voter quorum availability 测试，不是 disabled/scaffold 占位。

## 4. 是否发现不合理点 / 警告 / 风险

- 现有 `debug-tests` preset 仍然指向 `build/linux`，而按任务要求使用的 `debug-ninja-safe` 构建目录是 `build/linux/safe`。因此直接执行用户给定的：
  - `ctest --preset debug-tests -R "<T051测试名>" --output-on-failure`
  仍会得到 `No tests were found!!!`，这不是 T051 逻辑失败，而是测试 preset 与 safe 构建目录不一致。
- 本窗口尝试对 `build/linux/safe` 运行 T051 单测时，构建锁被占用，按任务要求未等待、未重试。
- 当前工作区 `tasks.md`、`tests/CMakeLists.txt`、部分 task-report 文件在本任务开始前就已有其他未提交变更。本任务只勾选了 `T051`，未改动其他任务项语义。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

原因：T051 只新增测试覆盖，不引入新的生产行为、持久化格式或跨平台风险登记需求。

## 6. 验证命令和结果

### 6.1 diff 检查

命令：

```bash
git diff -- tests/integrated_object_storage_quorum_test.cpp tests/CMakeLists.txt tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t051-5voter-quorum-availability-test.md
```

结果：

- PASS
- T051 相关新增变更集中在测试文件、任务勾选和任务报告

### 6.2 定向 configure + build

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_quorum' || echo "build lock busy, skip build in this window"
```

结果：

- PASS
- `integrated_object_storage_quorum` 目标成功 configure/build
- 本地日志：`tmp/test-logs/t051-build.log`

### 6.3 按用户给定 ctest preset 的一次尝试

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "^IntegratedObjectStorageQuorumTest\\.FiveVoterCommittedMembershipKeepsQuorumThreeAndAllowsCommitWithThreeReachableVoters$" --output-on-failure' || echo "build/test lock busy, skip test in this window"
```

结果：

- 未发现测试
- 摘要：`No tests were found!!!`
- 说明：`debug-tests` 指向 `build/linux`，与 `debug-ninja-safe` 的 `build/linux/safe` 不一致
- 本地日志：`tmp/test-logs/t051-ctest.log`

### 6.4 针对 safe 构建目录的单测尝试

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "^IntegratedObjectStorageQuorumTest\\.FiveVoterCommittedMembershipKeepsQuorumThreeAndAllowsCommitWithThreeReachableVoters$" --output-on-failure' || echo "build/test lock busy, skip test in this window"
```

结果：

- 构建锁被占用，本窗口未执行 build/test，待统一验证
- 本地日志：`tmp/test-logs/t051-ctest-safe.log`

## 7. 结论

- T051 代码实现已完成。
- 定向 build 已通过。
- 受构建锁占用影响，本窗口未完成 T051 单测执行，因此当前结论是“实现完成，验证待统一补跑”，不能在本报告中宣称测试已最终通过。
