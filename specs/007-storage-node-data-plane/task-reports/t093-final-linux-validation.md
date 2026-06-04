# T093 Final Linux Validation

## 修改文件

- `specs/007-storage-node-data-plane/task-reports/t093-final-linux-validation.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 运行了哪些最终 Linux 验证

- 先清理测试残留 snapshot / recovery / runtime 目录
- `cmake --build --preset debug-ninja-low-parallel`
- `CTEST_PARALLEL_LEVEL=1 ./test.sh --skip-configure --skip-build`
- 完整测试失败后，提取失败项并再次清理相关 runtime 目录
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "RaftSnapshotDiagnosisTest\\.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot" --output-on-failure`
- 在失败项仍未稳定后，再次删除该失败用例对应的 snapshot/runtime 目录
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "RaftSnapshotDiagnosisTest\\.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot" --output-on-failure`

## 清理了哪些 snapshot / runtime 路径

- 首轮清理前候选日志：`tmp/007/t093-cleanup-candidates.log`
- 首轮确认并清理：
  - `tmp/test-logs`
  - `build/linux/tests/raft_test_data`
  - `raft_test_data`
- 首轮清理日志：`tmp/007/t093-cleanup-actions.log`
- 失败项复测前候选日志：`tmp/007/t093-rerun-cleanup-before-failed-only.log`
- 失败项复测前确认并清理：
  - `build/linux/tests/raft_test_data`
- 失败项复测前清理日志：`tmp/007/t093-rerun-cleanup-confirmed.log`
- 最新单测重试前候选日志：`tmp/007/t093-retry-cleanup-candidates.log`
- 最新单测重试前确认并清理：
  - `build/linux/tests/raft_test_data/raft_snapshot_diagnosis_RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot_1780564388409_4073321182`
- 最新单测重试前清理日志：`tmp/007/t093-retry-cleanup-actions.log`

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel 2>&1 | tee tmp/007/t093-rerun-build.log`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ./test.sh --skip-configure --skip-build 2>&1 | tee tmp/007/t093-rerun-linux-all-single-thread.log`
  - FAIL
  - 说明：仓库当前 `test.sh` 默认全量入口会继续执行 configure/build；本次按真实入口行为记录
- 失败测试提取文件：`tmp/007/t093-rerun-failed-tests.txt`
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "RaftSnapshotDiagnosisTest\\.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot" --output-on-failure 2>&1 | tee tmp/007/t093-rerun-failed-only-single-thread.log`
  - FAIL
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -R "RaftSnapshotDiagnosisTest\\.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot" --output-on-failure 2>&1 | tee tmp/007/t093-retry-after-clean-single-thread.log`
  - PASS

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 完整测试失败项：
  - `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- 首次单独复测失败项：
  - `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- 最新删除失败用例目录后的单独复测结果：
  - PASS
- 已清理 snapshot / runtime 残留：
  - 是
- 单独复测是否单线程：
  - 是
- 完整测试错误摘要：
  - 恢复时跳过损坏的最新 snapshot 后，加载到更旧 snapshot（`index=60`），随后重放 committed tail 时在 `index=81` 触发 `MetadataStateMachine` 异常：`not found: object does not exist`
  - 抛出异常：`failed to replay committed log entries for node 3`
- 单独复测错误摘要：
  - 同样跳过损坏的最新 snapshot 后回退到旧 snapshot，但本次能完成 replay，最终 `LastAppliedIndex()` 为 `97`
  - 测试期望 `95`，实际得到 `97`
  - 关键断言：`Expected equality of these values: restarted_state_machine->LastAppliedIndex() ... 95u`
- 最新删除失败目录后的单独复测摘要：
  - 同一条测试在删除其对应 `raft_test_data/...RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot_*` 目录后，单线程单测复跑 PASS
- 当前根因判断：
  - 当前证据更接近“snapshot diagnosis 用例对运行目录/测试顺序/恢复时序存在敏感性”
  - 不能仅凭这次 targeted cleanup 后的 PASS，就宣称 T093 全量已经恢复通过
  - 但也不能继续把它简单定性成稳定的生产逻辑失败，因为删除对应失败目录后单测已通过
- 最后 50 行日志：
  - 完整测试：见 `tmp/007/t093-rerun-linux-all-single-thread.log` 末尾失败段
  - 单独复测：见 `tmp/007/t093-rerun-failed-only-single-thread.log` 末尾失败段
  - 最新删除失败目录后的 PASS 复测：见 `tmp/007/t093-retry-after-clean-single-thread.log`

## Windows 验证判断

- T093 是 Linux 当前环境最终验证
- 当前无 Windows 编译环境，不宣称 Windows PASS
- 既有 Windows 待验证项继续保留

## 是否通过 T093

- 否

## 007 当前 Linux 侧是否可以收口

- 不可以

## 当前任务发现的不合理点 / 警告 / 风险

- 本轮在“先清理残留、再单线程全量”条件下，原先 3 个阻塞项里已有 2 个恢复通过：
  - `MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached`
  - `RaftSnapshotRecoveryTest.StandaloneRestartFallsBackToOlderTrustedSnapshotWhenNewestSnapshotIsCorrupted`
- 完整单线程汇总里仍剩 1 个失败项：
  - `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot`
- 该失败项先后出现过两种失败形态，但在删除其失败用例对应目录后单独复测已 PASS
- 当前更像是 snapshot diagnosis 测试对运行目录、测试顺序或恢复时序敏感；在没有再次重跑整套 T093 全量前，仍不能直接把 T093 标记通过
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，本次继续按 007 实际任务与验证入口执行

## 是否修改高频文档及原因

- 未修改 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：T093 未通过，不能勾选完成

## common-risk-notes.md 读取结果

- 已读取 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- 本轮更新了 T093 风险描述，收口为“snapshot diagnosis 测试对运行目录/测试顺序/恢复时序敏感，targeted cleanup 后失败项可单测恢复 PASS”

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - 无新增风险类型；仅更新 T093 风险的最新验证结论
- 删除：无
- 保留：
  - Windows 文件语义待验证
  - 真实 metadata manifest coordination / Raft 提交未实现
  - RepairManager / RebalanceManager persistence 未实现
  - read-side repair 未实现
  - metadata / registry facts 新鲜度风险
  - repair/rebalance 后 manifest 更新与 cleanup 的一致性风险
  - timeout / cancellation 运行中传播边界
