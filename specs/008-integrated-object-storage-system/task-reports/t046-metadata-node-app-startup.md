# T046 任务报告

## 1. 修改了哪些文件

- `apps/metadata_node_app.cpp`
- `modules/cluster/cluster_config.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t046-metadata-node-app-startup.md`

说明：当前工作树里可能已存在其他任务的未提交改动；本任务只新增了 `metadata_node_app` 入口、勾选了 T046，并补充了本任务报告，没有回退或重写其他任务改动。

## 2. metadata_node_app thin startup 做了什么

- 新增 `metadata_node_app` CLI 入口，支持：
  - `--config <path>`
  - `--node_id <id>`
  - `--data_dir <path>`
  - `--listen <host:port>`
- 从统一 cluster config JSON 加载配置，并按 `role=metadata + node_id` 精确解析当前节点。
- 对当前 MetadataNode 的 `node_id`、`raft_id`、`snapshot_dir`、`initial_role` 做启动前校验。
- 额外校验当前节点的 `raft_id` 必须且只能出现在一类初始 membership 中，并与配置里的 `initial_role` 一致。
- 通过 `LoadOrCreateNodeIdentity(...)` 读写本地 `node.identity`，确保 `cluster_id / node_id / node_type / raft_id` 一致。
- 使用解析出的 metadata peers 装配现有 `raftdemo::NodeConfig` / `snapshotConfig`，再启动现有 `raftdemo::RaftNode`。
- 最小修复了 `LoadClusterConfigFromJsonFile(...)` 的 JSON 文本生命周期问题，避免统一 cluster config 在 app 启动时被不稳定解析。
- 成功启动时输出：
  - `cluster_id`
  - `node_id`
  - `raft_id`
  - `endpoint`
  - `data_dir`
  - `snapshot_dir`
  - `initial_role`
  - `initial_voters`
  - `initial_commit_quorum`
  - `identity_source`

## 3. 如何处理 cluster config、node.identity、raft_id、initial membership 和错误诊断

- `cluster config`
  - 通过 `LoadClusterConfigFromJsonFile(...)` 读取和校验统一配置。
  - 通过 `ResolveClusterNodeConfig(...)` 精确解析当前 MetadataNode，禁止 fallback 到默认 demo 节点。
- `node.identity`
  - 以 `cluster_id + node_id + node_type=metadata + raft_id` 作为期望身份。
  - 启动时允许首次创建 `node.identity`；若已存在但身份不匹配，则明确失败，不会静默复用其他节点目录。
- `raft_id`
  - 要求为正数。
  - 既用于 `node.identity` 校验，也用于装配 `raftdemo::NodeConfig.node_id` 和 peer 列表。
- `initial membership`
  - 只读计算初始 quorum 摘要用于校验和诊断。
  - 当前节点的 `raft_id` 必须属于 `voter_raft_ids` 或 `learner_raft_ids` 之一，且要与 `initial_role` 一致。
  - 没有把 app、ViewNode 或 live node 观测结果解释成 membership authority。
- `错误诊断`
  - 参数错误返回参数退出码。
  - 配置解析、节点解析、membership 边界错误返回配置退出码。
  - identity 冲突、损坏、durability 失败返回 identity/unsupported 退出码。
  - `RaftNode` 构造或 gRPC bind 失败返回 startup 退出码。

## 4. 是否确认不改变 Raft election / commit / membership 行为

确认未改变。

- 没有修改 `RaftNode`、`MetadataStateMachine`、`MetadataService` 的业务逻辑。
- 没有修改 election、commit、snapshot、recovery、membership 生产语义。
- app 只做启动装配、配置解析、身份校验和诊断输出。
- 初始 quorum 只作为只读校验与展示信息使用，没有改变运行时 quorum 规则。

## 5. 是否发现不合理点 / 警告 / 风险

- 现有 `RaftNode` 仍会在 `data_dir` 内维护旧的 `node_identity.txt`；本任务没有改写这条旧兼容路径，只在 app 层补上 008 需要的 `node.identity` 校验。后续如果要彻底统一 durable identity，建议单独任务收敛，避免在 T046 擅自改动核心语义。
- 当前 cluster config 里没有独立的 election timeout 配置字段，因此 app 只把 `heartbeat_interval` 和 `metadata_rpc_timeout` 映射到现有 `RaftNode` 可配置项，其余 election 窗口保持 `RaftNode` 现有默认值，避免在 app 层引入新的共识行为差异。
- 本任务没有新增 `--snapshot_dir` override；仍以 cluster config 中的 `snapshot_dir` 为准，保持 CLI 边界最小。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- apps/metadata_node_app.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md modules/cluster/cluster_config.h modules/cluster/cluster_config.cpp modules/cluster/node_identity.h modules/cluster/node_identity.cpp modules/raft/node/raft_node.h modules/raft/node/raft_node.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t046-metadata-node-app-startup.md
```

结果：本任务实际改动集中在 `apps/metadata_node_app.cpp`、`modules/cluster/cluster_config.cpp`、`tasks.md` 和本报告文件；其余文件仅作为 diff 观察范围，没有在本任务中修改。

### 最小构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target metadata_node_app'
```

结果：PASS

### 最小 smoke test

```bash
build/linux/safe/metadata_node_app --help
```

结果：PASS

```bash
timeout 3s build/linux/safe/metadata_node_app --config tmp/test-artifacts/t044-cluster.json --node_id meta-1
```

结果：PASS（进程成功打印 `metadata_node_app OK ...` 启动摘要，并在 `timeout` 截断后输出 `stopped`；shell 返回码为 `124`，属于受控超时包装，不是启动失败）
