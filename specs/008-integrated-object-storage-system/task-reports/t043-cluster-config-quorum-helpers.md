# T043 Cluster Config Quorum Helpers 报告

## 1. 修改了哪些文件

- `modules/cluster/cluster_config.cpp`
- `modules/cluster/cluster_config.h`
- `modules/cluster/module-notes.md`
- `tests/cluster_config_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t043-cluster-config-quorum-helpers.md`

未修改：

- `proto/*`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. quorum calculation helpers 做了什么

本任务只在 cluster config 层补充 quorum helper，没有改动 Raft 运行时逻辑。

具体新增/补充：

- 在 `cluster_config.h` 中新增：
  - `InitialRaftQuorumSummary`
  - `InitialRaftQuorumComputationResult`
- 新增 `ComputeInitialRaftQuorum(const InitialRaftMembershipConfig &membership)`
  - 只基于 initial voter membership 计算 majority quorum
  - 返回 `voter_count`、`election_quorum`、`commit_quorum`、`voter_raft_ids`
  - 对以下场景返回可诊断错误：
    - 空 voter 集合
    - learner-only membership
    - 偶数 voter 数
    - 重复 voter
    - voter / learner 重叠
    - 非法 raft_id
- 新增 `ComputeInitialRaftQuorum(const ClusterConfig &config)`
  - 先校验 `ClusterConfig`
  - 再基于 `config.initial_raft_membership` 计算 quorum
- 保留现有 `ComputeInitialRaftQuorumSize(...)`
  - 作为轻量包装接口继续存在
  - 当 membership 非法时，`ComputeInitialRaftQuorumSize(const InitialRaftMembershipConfig &)` 返回 `0`
- 在 `cluster_config_test.cpp` 中补充 quorum helper 测试
  - 验证 1/3/5/7 voter 分别得到 1/2/3/4 quorum
  - 验证空 membership、learner-only、重复 voter、重叠 membership 均返回诊断错误

## 3. 是否确认 helper 不改变 Raft election / commit 行为

已确认不改变。

- helper 只读取 `initial_raft_membership`
- helper 只服务于配置校验、测试和启动前诊断
- 没有修改 `RaftNode` 的 election 行为
- 没有修改 `RaftNode` 的 commit 行为
- 没有实现动态 membership change
- 没有把 helper 变成运行时 membership authority

## 4. 是否发现不合理点 / 警告 / 风险

发现的注意点：

- 当前 `ComputeInitialRaftQuorumSize(std::size_t voter_count)` 仍然是一个无诊断的轻量公式接口；带诊断的边界校验集中在新加的 `ComputeInitialRaftQuorum(...)`。后续如果 app startup 或 CLI 需要对用户暴露错误原因，应优先使用带结果对象的 helper。
- 当前工作区在本任务开始前已存在其他未提交改动；本次只把 `T043` 从 `[ ]` 改为 `[X]`。如果 `git diff` 同时出现 `T042`、`T044` 或其他 task-report 变化，应视为既有工作区状态，而不是本任务额外扩展。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 6. 验证命令和结果

执行的验证命令：

```bash
git diff -- modules/cluster/cluster_config.cpp modules/cluster/cluster_config.h modules/cluster/module-notes.md tests/cluster_config_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t043-cluster-config-quorum-helpers.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target cluster_config_test'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R cluster_config --output-on-failure'
```

结果：

- `cluster_config_test` 构建通过
- `ctest --test-dir build/linux/safe -R cluster_config --output-on-failure`
  - 10/10 PASS
  - 与 T043 直接相关的新增通过用例：
    - `cluster_config_quorum_helper_test.computes_majority_quorum_for_1_3_5_7_initial_voters`
    - `cluster_config_quorum_helper_test.rejects_empty_or_learner_only_membership_with_diagnostics`
    - `cluster_config_quorum_helper_test.rejects_duplicate_and_overlapping_membership_entries`

本地日志文件：

- `tmp/test-logs/t043-build.log`
- `tmp/test-logs/t043-ctest-safe.log`
