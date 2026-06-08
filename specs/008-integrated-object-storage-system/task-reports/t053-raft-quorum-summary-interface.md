# T053 - Raft quorum summary 只读接口

## 1. 修改文件

- `modules/raft/node/raft_node.h`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t053-raft-quorum-summary-interface.md`

## 2. RaftNode read-only committed membership / quorum summary 接口做了什么

本任务只在 `RaftNode` 头文件中新增了只读诊断契约，没有实现逻辑：

- 新增 `CommittedMembershipRole`
  - 表达本节点在已提交 membership 中的诊断角色边界：`kVoter`、`kLearner`、`kNonMember`、`kUnknown`
- 新增 `CommittedMembershipQuorumSummary`
  - 暴露已提交 membership 对应的：
    - `committed_log_index`
    - `committed_term`
    - `voter_ids`
    - `learner_ids`
    - `voter_count`
    - `learner_count`
    - `quorum_size`
    - `local_role`
  - 结构体注释明确说明 quorum 必须基于 committed membership，而不是 live nodes，也不是 ViewNode observation
- 在 `RaftNode` 公有接口中新增：
  - `CommittedMembershipQuorumSummary GetCommittedMembershipQuorumSummary() const;`
  - 接口注释明确这是只读诊断接口，不提供任何可变入口

## 3. 是否保持只读诊断接口、不改变 election / commit 行为

是。

- 仅修改 `raft_node.h` 的接口声明
- 未修改 `raft_node.cpp`
- 未修改 election、commit、membership change、日志复制、snapshot、recovery 逻辑
- 未实现 AddRaftNode / RemoveRaftNode / PromoteLearner
- 未让 ViewNode 或其他调用方获得任何 membership 修改能力

## 4. 是否发现不合理点 / 警告 / 风险

- 当前仓库里多数派计算仍直接依赖现有节点配置与 peer 数量；本任务没有触碰该行为，这与 T053 只定义接口的边界一致
- `CommittedMembershipQuorumSummary` 已预留 learner 边界和本地角色诊断，但真实来源与填充逻辑需要在 T054 中实现并校准到“committed membership authority”
- 当前接口中的成员 ID 字段命名使用 `voter_ids` / `learner_ids`，保持与现有 `RaftNode` 语境兼容；如果后续需要严格区分 `node_id` 与 `raft_id`，应在 T054/T055 结合实现和映射时再次核对

## 5. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

未修改。

- `common-risk-notes.md`：未修改
- `risk-register.md`：未修改

## 6. 验证命令和结果

### diff 检查

命令：

```bash
git diff -- modules/raft/node/raft_node.h \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t053-raft-quorum-summary-interface.md
```

结果：

- `git diff -- ...` 已确认跟踪文件改动只包含 `raft_node.h` 接口声明和 `tasks.md` 的 T053 勾选
- `git status --short -- ...` 显示任务报告文件为新增 `??`
- `git diff --no-index -- /dev/null specs/008-integrated-object-storage-system/task-reports/t053-raft-quorum-summary-interface.md` 已确认新增报告内容正确

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
