# T054 - Raft quorum summary 只读实现

## 1. 修改文件

- `modules/raft/node/raft_node.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t054-raft-quorum-summary-impl.md`

## 2. committed membership / quorum summary 实现做了什么

本任务只在 `RaftNode` 的 `.cpp` 中实现了 T053 暴露的只读接口：

- 实现 `RaftNode::GetCommittedMembershipQuorumSummary() const`
- 返回：
  - `committed_log_index = commit_index_`
  - `committed_term = TermAtIndexLocked(commit_index_)`
  - `voter_ids`
  - `learner_ids`
  - `voter_count`
  - `learner_count`
  - `quorum_size`
  - `local_role`
- 新增本地 helper `ComputeCommittedVoterQuorumSize()`，按 `floor(voter_count / 2) + 1` 计算 quorum
- 对 `voter_ids` 做排序与去重，确保诊断输出稳定

当前阶段 `RaftNode` 运行态内部还没有独立的 runtime membership authority，也没有 learner membership 输入，因此本实现遵循现有一阶段边界：

- committed voter 集合从 `RaftNode` 当前已知配置 `self + peers` 生成
- `learner_ids` 保持为空
- quorum 只由 committed voter 总数计算
- 不读取 live 节点数
- 不读取 ViewNode 观测状态
- 不把 registered-only 节点或观测节点计入 voter quorum

## 3. 是否确认不改变 election / commit / membership 行为

是。

- 仅新增只读诊断实现
- 未修改 election 路径
- 未修改 commit 推进逻辑
- 未修改 replication / snapshot / recovery 逻辑
- 未实现 AddRaftNode / RemoveRaftNode / PromoteLearner
- 未让 ViewNode 或外部调用方获得 membership 修改能力

## 4. 是否发现不合理点 / 警告 / 风险

- 当前仓库的实际 election / commit 多数派计算仍沿用现有 `config_.peers.size() + 1` 路径；T054 没有改变它，只是提供并行的只读诊断摘要
- 当前 `RaftNode` 运行态没有单独的 `raft_id / learner membership` 输入，因此本实现中的成员 ID 使用 `NodeConfig.node_id / PeerConfig.node_id` 语义；如果后续 MetadataNode 启动路径把 `node_id` 与 `raft_id` 明确拆开，需要在 T055/T056 或后续 membership 实体接入时再次校准
- `learner_ids` 当前为空是有意保守处理，避免误把未知节点算入 voter quorum；如果后续引入 committed learner authority，应扩展数据来源而不是改写诊断用途

## 5. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

未修改。

- `common-risk-notes.md`：未修改
- `risk-register.md`：未修改

## 6. 验证命令和结果

### diff 检查

命令：

```bash
git diff -- modules/raft/node/raft_node.cpp \
  modules/raft/node/raft_node.h \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t054-raft-quorum-summary-impl.md
```

结果：

- `git diff -- ...` 显示 `raft_node.cpp` 的 T054 实现，以及工作区中已存在的 `raft_node.h` / `tasks.md` 前序未提交改动
- 本任务实际新增的跟踪文件改动是：
  - `modules/raft/node/raft_node.cpp` 的只读 quorum summary 实现
  - `specs/008-integrated-object-storage-system/tasks.md` 中 T054 从 `[ ]` 改为 `[X]`
- `modules/raft/node/raft_node.h` 本任务未修改；其 diff 来自前序 T053 未提交改动
- `git status --short -- ...` 显示任务报告文件为新增 `??`
- `git diff --no-index -- /dev/null specs/008-integrated-object-storage-system/task-reports/t054-raft-quorum-summary-impl.md` 已确认新增报告内容正确

### 最小构建验证

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core' \
  || echo "build lock busy, skip build in this window"
```

结果：

- PASS
- `cmake --preset debug-ninja-safe` 配置成功
- `cmake --build --preset debug-ninja-safe --target raft_core` 构建成功

### 最小测试验证

结果：

- 当前任务未新增或修改测试
- 当前仓库尚无单独的 quorum summary 专用测试 target 可直接最小化运行
