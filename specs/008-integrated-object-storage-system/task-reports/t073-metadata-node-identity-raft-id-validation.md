# T073 MetadataNode config-generated node_id / raft_id validation 报告

## 1. 修改了哪些文件

- `apps/metadata_node_app.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t073-metadata-node-identity-raft-id-validation.md`

未修改：

- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `specs/008-integrated-object-storage-system/contracts/app-cli.md`
- `common-risk-notes.md`
- `risk-register.md`
- `proto/`

## 2. metadata_node_app 的 config-generated node_id / raft_id validation 接入做了什么

- 在 `ResolveStartupConfig(...)` 中继续使用 `ResolveClusterNodeConfig(...)` 解析 MetadataNode，但把解析结果收紧为“配置生成身份”基线。
- 新增 `ValidateLocalOverrideSafety(...)`，对以下边界做显式校验：
  - `node_id` 不为空
  - `raft_id` 必须为正数
  - `data_dir` / `snapshot_dir` 不为空
  - `listen_endpoint` 必须是合法 `host:port`
  - `initial_role` 只能是 `voter` 或 `learner`
- 对 `--listen` 增加显式拒绝语义：如果 override 和 config-generated endpoint 不一致，直接失败，不允许让同一个 `raft_id` 在不同运行时 endpoint 启动。
- 对 `--data_dir` 增加显式拒绝语义：如果 override 和 config-generated `data_dir` 不一致，直接失败，不允许把同一个 `node_id / raft_id` 的 durable identity 移到别的目录。
- 保留 `ValidateLocalMembershipBoundary(...)`，继续要求本节点 `raft_id` 必须且只能属于一个初始 membership 角色集合，并且与 `initial_role` 一致。
- 保留 `EnsureNodeIdentity(...)` 的 `source=config_generator` 期望，继续拒绝把 `ViewNodeAllocator` 或 `ExplicitOverride` 来源的 `node.identity` 当作 MetadataNode 的 Raft 身份。
- 在帮助输出和代码注释中明确：ViewNode registration 只上报观测信息，不能分配、覆盖或漂移 MetadataNode 的 `raft_id`。

## 3. 如何处理配置生成身份、本地 identity、override、membership 边界和错误诊断

- 配置生成身份：
  - `node_id`、`raft_id`、`endpoint`、`data_dir`、`snapshot_dir`、`initial_role` 全部先从 cluster config 解析。
  - app 以这些字段作为 MetadataNode 启动的唯一身份基线。

- 本地 `node.identity`：
  - 继续通过 `LoadOrCreateNodeIdentity(...)` 校验/创建。
  - 期望字段保持为 `cluster_id + node_id + node_type=metadata + raft_id + source=config_generator`。
  - 如果已有 identity 来源不是 `config_generator`，或 `node_id/raft_id` 不匹配，会沿用现有 node identity 诊断明确失败。

- override 边界：
  - `--node_id` 只允许选择配置里存在的 MetadataNode；解析后若和最终身份不一致直接失败。
  - `--listen` 如果改变 config-generated endpoint，直接返回配置错误。
  - `--data_dir` 如果改变 config-generated durable identity 目录，直接返回配置错误。
  - 这样可以阻断本地 override 把同一个 `raft_id` 绑定到不同 endpoint 或不同 durable state 的漂移路径。

- membership 边界：
  - 仍只读使用 `ComputeInitialRaftQuorum(...)` 和 `initial_raft_membership` 做边界校验。
  - 不接受“配置外 raft_id”、“角色与 membership 不一致”或“raft_id 同时属于多个角色集合”。
  - 不根据 ViewNode 状态、注册结果或 liveness 改写 membership 结论。

- 错误诊断：
  - 配置错误统一走 `metadata_node_app config error: ...`
  - `--listen` 和 `--data_dir` 漂移拒绝都输出 config-generated 基线值、override 值和拒绝原因
  - `node.identity` 冲突继续走现有 identity 诊断和退出码映射

## 4. 是否确认不改变 Raft election / commit / membership 行为

确认未改变。

- 未修改 `RaftNode`、`MetadataService`、`MetadataStateMachine`、snapshot/recovery 逻辑。
- 未修改 Raft quorum、election、commit、membership 生产语义。
- 未实现 `AddRaftNode / RemoveRaftNode / PromoteLearner`。
- app 侧只加强启动前身份校验和错误展示。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `metadata_node_app` 对 `--listen` / `--data_dir` 的 MetadataNode 语义已经从“可本地覆盖”收紧为“只要偏离 config-generated 身份就拒绝”。这是 T073 需要的安全边界，但也意味着这两个参数对 MetadataNode 的用途基本退化为“显式重复配置值”或排障提示。
- `tasks.md` 当前工作树里已存在与本任务无关的 `T072`、`T074`、`T075` 勾选差异；本任务只额外把 `T073` 从 `[ ]` 改为 `[X]`，未处理这些既有差异。
- 现有正常启动 smoke 依赖 `tmp/test-artifacts/t044-cluster.json`。这是本地验证工件，不是新的源码依赖。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 7. 验证命令和结果

执行命令：

```bash
git diff -- apps/metadata_node_app.cpp modules/cluster/cluster_config.h modules/cluster/cluster_config.cpp modules/cluster/node_identity.h modules/cluster/node_identity.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t073-metadata-node-identity-raft-id-validation.md
git diff --check -- apps/metadata_node_app.cpp
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target metadata_node_app'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/metadata_node_app --help'
flock -n /tmp/cqupt_raft_build.lock -c 'stdbuf -oL -eL timeout --preserve-status -s INT 3s ./build/linux/safe/metadata_node_app --config tmp/test-artifacts/t044-cluster.json --node_id meta-1'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/metadata_node_app --config tmp/test-artifacts/t044-cluster.json --node_id meta-1 --listen 127.0.0.1:29999'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/metadata_node_app --config tmp/test-artifacts/t044-cluster.json --node_id meta-1 --data_dir tmp/t073-drift-data'
```

结果：

- `git diff --check`: PASS
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target metadata_node_app`: PASS
- `./build/linux/safe/metadata_node_app --help`: PASS
- 正常启动 smoke：PASS
  - 成功打印 `metadata_node_app OK cluster_id=... node_id=meta-1 raft_id=1 ...`
  - 进程被 `timeout` 受控结束并打印 `stopped`
- `--listen` 漂移拒绝：PASS
  - 输出：
    - `metadata_node_app config error: --listen override is rejected for MetadataNode: config-generated endpoint=127.0.0.1:18101 override=127.0.0.1:29999; refusing to start raft_id=1 on a different runtime endpoint`
  - 退出码：`3`
- `--data_dir` 漂移拒绝：PASS
  - 输出：
    - `metadata_node_app config error: --data_dir override is rejected for MetadataNode: config-generated data_dir=tmp/test-artifacts/t044-cluster-root/metadata/meta-1/data override=tmp/t073-drift-data; refusing to move durable node_id/raft_id state to another directory`
  - 退出码：`3`
