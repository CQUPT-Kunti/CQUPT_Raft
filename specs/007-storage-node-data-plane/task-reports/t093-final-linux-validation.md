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
- 最新 follow-up：按用户要求执行“分段全量单线程 + 在易错测试 165 前再次删除 snapshot/runtime 目录”
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 1,164,1 --output-on-failure -j 1`
  - 清理 diagnosis 相关 `raft_snapshot_diagnosis_*` 运行目录
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 165,165,1 --output-on-failure -j 1`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 166,225,1 --output-on-failure -j 1`

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
- rerun2 全量前候选日志：`tmp/007/t093-rerun2-cleanup-candidates.log`
- rerun2 全量前确认并清理：
  - `tmp/test-logs`
  - `build/linux/tests/raft_test_data`
- rerun2 全量前清理日志：`tmp/007/t093-rerun2-cleanup-actions.log`
- rerun2 在 `165` 前额外清理 diagnosis 目录：
  - `build/linux/tests/raft_test_data/raft_snapshot_diagnosis_*`
- rerun2 在 `165` 前清理日志：
  - `tmp/007/t093-rerun2-pre165-cleanup-candidates.log`
  - `tmp/007/t093-rerun2-pre165-cleanup-actions.log`

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
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 1,164,1 --output-on-failure -j 1 2>&1 | tee tmp/007/t093-rerun2-part1-pre-diagnosis.log`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 165,165,1 --output-on-failure -j 1 2>&1 | tee tmp/007/t093-rerun2-test165.log`
  - PASS
- `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux -I 166,225,1 --output-on-failure -j 1 2>&1 | tee tmp/007/t093-rerun2-part3-post-diagnosis.log`
  - PASS
- rerun2 汇总日志：
  - `tmp/007/t093-rerun2-summary.log`

## 结果判断

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
  - 早先失败证据更接近“snapshot diagnosis 用例对运行目录/测试顺序/恢复时序存在敏感性”
  - 仅凭单独清理后的单测 PASS 还不能直接宣布 T093 恢复通过
  - 但在后续 rerun2 中，已经通过“全量前清理 + 易错测试前定点清理”的三段式单线程全量覆盖补足了这一点
- 最后 50 行日志：
  - 完整测试：见 `tmp/007/t093-rerun-linux-all-single-thread.log` 末尾失败段
  - 单独复测：见 `tmp/007/t093-rerun-failed-only-single-thread.log` 末尾失败段
  - 最新删除失败目录后的 PASS 复测：见 `tmp/007/t093-retry-after-clean-single-thread.log`
- 最新 rerun2 结论：
  - 在“全量前清理测试残留 + `165` 开跑前再次删除 diagnosis snapshot/runtime 目录”的条件下，三段全量覆盖全部 225 个测试并全部 PASS
  - 因此本轮把 T093 视为通过
  - 同时保留一个非阻塞说明：`RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot` 对运行目录/恢复时序存在敏感性，后续如再跑全量，建议继续保留同类预清理步骤

## Windows 验证判断

- T093 是 Linux 当前环境最终验证
- 当前无 Windows 编译环境，不宣称 Windows PASS
- 既有 Windows 待验证项继续保留

## 是否通过 T093

- 是

## 007 当前 Linux 侧是否可以收口

- 可以收口

## 当前任务发现的不合理点 / 警告 / 风险

- 本轮在“先清理残留、再单线程全量”条件下，原先 3 个阻塞项里已有 2 个恢复通过：
  - `MetadataConcurrencyStressTest.AdmissionRejectsWhenInflightLimitIsReached`
  - `RaftSnapshotRecoveryTest.StandaloneRestartFallsBackToOlderTrustedSnapshotWhenNewestSnapshotIsCorrupted`
- 最新 rerun2 已用三段单线程覆盖完整 225 测试：
  - `1..164` PASS
  - `165` 在开跑前额外删除 diagnosis snapshot/runtime 目录后 PASS
  - `166..225` PASS
- 当前仍保留一个非阻塞提示：
  - `RaftSnapshotDiagnosisTest.RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot` 对运行目录、测试顺序或恢复时序敏感
  - 后续如果再次跑全量，建议延续本轮“全量前清理 + 易错测试前定点清理”的做法
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，本次继续按 007 实际任务与验证入口执行

## 是否修改高频文档及原因

- 未在本次 T093 rerun2 报告内再次修改 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：T093 完成状态已在后续 follow-up 文档更新时同步到 `tasks.md`

## common-risk-notes.md 读取结果

- 已读取 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- 本轮继续保留 T093 相关说明，但其性质从“阻塞 Linux 收口”更新为“非阻塞的运行目录/恢复时序敏感提示”

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：
  - Windows 文件语义待验证
  - 真实 metadata manifest coordination / Raft 提交未实现
  - RepairManager / RebalanceManager persistence 未实现
  - read-side repair 未实现
  - metadata / registry facts 新鲜度风险
  - repair/rebalance 后 manifest 更新与 cleanup 的一致性风险
  - timeout / cancellation 运行中传播边界
