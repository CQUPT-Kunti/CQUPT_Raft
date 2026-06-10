# T061 任务报告

## 1. 修改了哪些文件

- `apps/metadata_node_app.cpp`
- `CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t061-metadata-node-registration-heartbeat.md`

本任务未修改：

- `modules/view/view_client.h`
- `modules/view/view_client.cpp`
- `specs/008-integrated-object-storage-system/contracts/app-cli.md`
- `common-risk-notes.md`
- `risk-register.md`
- `proto/`
- `tests/`

说明：

- `CMakeLists.txt` 的修改是编译级必要修正。`metadata_node_app` 在接入 `ViewNodeClient` 后，target 需要显式链接 `view_proto`；否则会在链接阶段出现 `view::ViewNodeService` 和 `view.pb` 符号缺失。
- `tasks.md` 当前工作树里可能存在其他既有任务勾选差异；本任务只额外将 `T061` 从 `[ ]` 改为 `[X]`。

## 2. metadata_node_app 的 ViewNode registration / heartbeat 接入做了什么

- 在 `metadata_node_app` 启动完成 `RaftNode` 后，基于 cluster config 中的 `view_nodes` 构造 `ViewNodeClient` 列表。
- 新增了 MetadataNode -> ViewNode 的 observation 请求构造逻辑，注册/heartbeat 上报字段包括：
  - `cluster_id`
  - `node_id`
  - `node_type=metadata`
  - `endpoint` / `control_plane_endpoint`
  - `data_dir_fingerprint`
  - 健康状态（当前实现保守上报为 `HEALTHY` + `LOW`）
  - MetadataNode 观测信息：
    - `raft_id`
    - `raft_role`
    - `membership_state`
    - `leader_hint`
    - `observed_term`
    - `commit_index`
    - `membership_epoch`
- `raft_role` 和 `leader_hint` 只从 `RaftNode::GetStatusSnapshot()` 读取；`membership_state` 只从 `RaftNode::GetCommittedMembershipQuorumSummary()` 读取，并用 `initial_role` 做保守 fallback。
- app 启动后会先对每个 configured ViewNode 做一次注册尝试，然后启动后台 heartbeat 线程：
  - 未注册成功的 endpoint 会在后续循环中继续尝试注册；
  - 已注册成功的 endpoint 会按 `timeouts.heartbeat_interval` 周期发送 heartbeat；
  - `HeartbeatNode` 返回 `NotFound` / `Conflict` 时，会把该 endpoint 状态重置为“需要重新注册”。
- 为 heartbeat sequence 增加了每个 ViewNode endpoint 独立的本地计数器，避免旧 heartbeat 覆盖新观测。
- ViewNode transport failure、注册冲突、未注册、诊断 warning 都会以明确的 app 输出打印出来，并对重复错误做抑制，避免每个 heartbeat 周期刷同样的日志。
- 当 ViewNode 重新可达并恢复注册/heartbeat 成功时，会输出一条 `view recovered` 提示。

## 3. 如何保持 ViewNode observation 与 Raft membership authority 的边界

- `metadata_node_app` 只读取 `RaftNode` 的只读快照和只读 quorum 摘要，不修改任何 election、commit 或 membership 状态。
- ViewNode 注册/heartbeat 失败不会阻止 `RaftNode` 启动，也不会触发 quorum 降级、leader 选择或 membership 修正。
- 上报给 ViewNode 的 `membership_state` 只是 observation；真实 voter/learner authority 仍来自已提交 membership。
- `leader_hint` 只是当前 MetadataNode 对 leader 的观测提示，不会被 app 当作强一致 leader 结论来写回系统。
- 没有把 object manifest、对象可见性、payload 或 chunk 数据带入 ViewNode。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 `metadata_node_app` 的健康/负载上报仍是保守静态值：`HEALTHY`、`LOW`、零负载。T061 的重点是注册/heartbeat 接入，这里没有越界把更深的 runtime metrics 采集塞进 app；后续若要增强观测粒度，应通过明确任务补齐。
- 当前启动 smoke 中，如果没有真实 ViewNode 进程，app 会打印清晰的 `Connection refused` 诊断，但仍继续启动。这是刻意保持 ViewNode non-authority 边界的结果，不是降级失败。
- 本次为了完成最小构建，新增了 `metadata_node_app -> view_proto` 的 target 链接。这个改动不改变业务语义，但说明此前 `T048` 报告中的 app-specific link boundary 在当前工作树并未完全落地。
- 受控启动 smoke 会打印现有 `RaftNode` 的日志（例如 election retry）。本任务没有修改这些核心行为；它们只是单节点 smoke 场景下没有 peer 的正常表现。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md；如未修改，明确说明未修改

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 6. 验证命令和结果

### Diff 检查

```bash
git diff -- apps/metadata_node_app.cpp modules/view/view_client.h modules/view/view_client.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t061-metadata-node-registration-heartbeat.md
```

结果：

- 核心实现改动集中在 `apps/metadata_node_app.cpp`
- `modules/view/view_client.h/.cpp` 未修改
- `contracts/app-cli.md` 未修改
- 另有一个编译级必要的 `CMakeLists.txt` 链接修正，需要单独记录

补充 diff：

```bash
git diff -- CMakeLists.txt apps/metadata_node_app.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t061-metadata-node-registration-heartbeat.md
```

结果：PASS，确认本任务实际落地文件与报告一致。

### diff 格式检查

```bash
git diff --check -- CMakeLists.txt apps/metadata_node_app.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t061-metadata-node-registration-heartbeat.md
```

结果：PASS。

### 最小构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target metadata_node_app'
```

结果：PASS。

- 首次尝试暴露出链接缺少 `view_proto` 的问题；
- 补上 `metadata_node_app -> view_proto` 之后重新构建通过。

### 最小 smoke test

帮助输出：

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/metadata_node_app --help'
```

结果：PASS，成功输出 `Usage`。

受控启动：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'stdbuf -oL -eL timeout --preserve-status -s INT 3s ./build/linux/safe/metadata_node_app --config tmp/test-artifacts/t044-cluster.json --node_id meta-1'
```

结果：PASS。

关键输出包含：

- `metadata_node_app view warning: endpoint=127.0.0.1:18001 stage=register ... Connection refused`
- `metadata_node_app OK cluster_id=t044-cluster node_id=meta-1 raft_id=1 ...`
- 最终 `stopped`

这说明：

- ViewNode 不可用时，注册诊断清晰；
- 但 MetadataNode 仍能按 non-authority 边界继续启动；
- app 生命周期与后台 registration / heartbeat 线程都能受控退出。
