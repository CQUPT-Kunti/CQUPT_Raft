# T052 任务报告：ViewNode-registered Raft node not counted as voter 测试

## 1. 修改了哪些文件

- `tests/integrated_object_storage_quorum_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t052-viewnode-registered-node-not-voter-test.md`

本任务未修改：

- `tests/CMakeLists.txt`
- `tests/support/integrated_cluster_test_utils.h`
- `specs/008-integrated-object-storage-system/common-risk-notes.md`
- `specs/008-integrated-object-storage-system/risk-register.md`

## 2. T052 的 ViewNode-registered node not counted as voter 测试做了什么

在 `tests/integrated_object_storage_quorum_test.cpp` 中新增：

- `IntegratedObjectStorageQuorumTest.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet`

测试覆盖内容：

1. 启动一个 3-voter 的真实 Raft MetadataNode 测试集群，确认先形成单 leader。
2. 在 committed membership 正常时先提交 `CreateBucket`。
3. 停掉 1 个真实 voter，只保留 2 个真实可达 voter。
4. 独立构造一个 `ViewNodeRegistry`，向其中注册 4 个 metadata 节点观测记录：
   - 3 个原有 metadata 节点的观测记录
   - 1 个额外的 metadata 节点观测记录
5. 把这个额外节点在 ViewNode 中显式标记为 `membership_state = VOTER`，并给出 `raft_id = 4`，模拟“ViewNode 看起来像多了一个 voter” 的强边界场景。
6. 读取 `GetClusterView`，断言 ViewNode 确实暴露了这个额外的 observed voter。
7. 在真实 Raft 集群中继续仅依赖剩余 2 个 committed voter 执行：
   - `CreateObject`
   - `CommitObject`
8. 断言：
   - 操作必须成功
   - 对象最终在存活节点上一致变为 `COMMITTED`
   - 这证明 ViewNode 注册结果即使显示为 `VOTER`，也没有扩大 committed membership，也没有把 quorum 从 2 提高到 3

这个测试锁定的边界是：

- ViewNode 的 `REGISTERED/JOINING/LEARNER/VOTER/DOWN` 只是观测信息
- 额外注册到 ViewNode 的 metadata 节点不能被算进 Raft voter 集合
- quorum 仍然必须来自 Raft 已提交 membership，而不是来自 ViewNode 观测快照

## 3. 是否有 disabled/scaffold 测试

没有。

本任务新增的是可编译、可执行、已实际验证通过的真实边界测试。

## 4. 是否发现不合理点 / 警告 / 风险

- 现有 `debug-tests` preset 仍然指向 `build/linux`，而按任务要求使用的 `debug-ninja-safe` 构建目录是 `build/linux/safe`。因此直接执行：
  - `ctest --preset debug-tests -R "<T052测试名>" --output-on-failure`
  仍会得到 `No tests were found!!!`，这不是 T052 测试失败，而是 preset 与 safe 构建目录不一致。
- 当前工作区在本任务开始前就已有其他未提交改动，包括：
  - `T050/T051`
  - `T053/T054/T056`
  - `tests/CMakeLists.txt`
  本任务只新增 T052 测试、T052 勾选和 T052 报告，没有改动其他任务逻辑。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

原因：T052 只新增观测边界测试，不涉及新的生产风险登记或持久化/协议变更。

## 6. 验证命令和结果

### 6.1 diff 检查

命令：

```bash
git diff -- tests/integrated_object_storage_quorum_test.cpp tests/CMakeLists.txt tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t052-viewnode-registered-node-not-voter-test.md
```

结果：

- PASS
- T052 相关新增内容集中在测试文件、任务勾选和任务报告

### 6.2 定向 configure + build

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target integrated_object_storage_quorum' || echo "build lock busy, skip build in this window"
```

结果：

- PASS
- `integrated_object_storage_quorum` 成功 configure/build
- 本地日志：`tmp/test-logs/t052-build.log`

### 6.3 按用户给定 ctest preset 的一次尝试

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "^IntegratedObjectStorageQuorumTest\\.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet$" --output-on-failure' || echo "build/test lock busy, skip test in this window"
```

结果：

- 未发现测试
- 摘要：`No tests were found!!!`
- 说明：`debug-tests` 指向 `build/linux`，与 `debug-ninja-safe` 的 `build/linux/safe` 不一致
- 本地日志：`tmp/test-logs/t052-ctest.log`

### 6.4 针对 safe 构建目录的单测验证

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R "^IntegratedObjectStorageQuorumTest\\.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet$" --output-on-failure' || echo "build/test lock busy, skip test in this window"
```

结果：

- PASS
- 通过测试：
  - `IntegratedObjectStorageQuorumTest.ViewNodeRegisteredObservedVoterDoesNotExpandCommittedRaftVoterSet`
- 总耗时：约 1.22s
- 本地日志：`tmp/test-logs/t052-ctest-safe.log`

## 7. 结论

- T052 代码实现已完成。
- 定向 build 已通过。
- T052 定向单测已通过。
- 当前可以把 T052 视为完成并进入后续 US5 工作。
