# T047 任务报告：storage_node_app thin startup

## 1. 修改了哪些文件

- `apps/storage_node_app.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t047-storage-node-app-startup.md`

## 2. storage_node_app thin startup 做了什么

- 新增 `storage_node_app` 入口，实现 `--config`、`--node_id`、`--data_dir`、`--listen` 参数解析和帮助输出。
- 复用 `LoadClusterConfigFromJsonFile(...)` 加载统一 cluster config，并按 `StorageNode` 角色解析当前节点配置。
- 支持两类受控启动路径：
  - 命中已命名 `StorageNode` 配置时，直接按配置中的 `node_id/endpoint/data_dir/capacity` 启动。
  - 当配置中只有单个未命名 `StorageNode` 时，允许使用显式 `--node_id` 绑定该条目，并把 identity source 标记为 `kExplicitOverride`。
- 复用 `LoadOrCreateNodeIdentity(...)` 完成本地 `node.identity` 的 load-or-create 与冲突诊断，保持只校验身份，不改变集群 authority。
- 装配 `LocalDiskChunkStore`、`StorageNodeRegistry`、`StorageNodeService` 和 gRPC `Server`，并输出 `cluster_id/node_id/endpoint/data_dir/capacity/identity_path` 等启动诊断信息。
- 用信号处理和 `server->Shutdown()` 建立最小生命周期边界，不在 app 中引入 chunk store 业务逻辑、placement 或 heartbeat loop。

## 3. 如何处理 cluster config、node.identity、capacity、data_dir 和错误诊断

- `cluster config`
  - 启动前必须先通过 `LoadClusterConfigFromJsonFile(...)` 的基础校验。
  - 解析失败、角色不匹配、`node_id` 不存在、endpoint 非法等情况返回非零退出码并打印明确错误。
- `node.identity`
  - 期望身份固定为 `cluster_id + node_id + node_type=storage`，禁止携带 `raft_id`。
  - 已有 identity 与配置不一致、文件损坏、durability 不满足时显式失败，不会静默覆盖。
- `capacity`
  - 要求 `capacity_bytes > 0`，否则视为配置错误并拒绝启动。
  - 启动时把容量边界同步写入本地 `StorageNodeRegistryFacts`，仅用于本地诊断种子，不成为 metadata/control-plane authority。
- `data_dir`
  - 支持 `--data_dir` 本地测试 override，但会校验不与 ViewNode、MetadataNode、其它 StorageNode 的 `data_dir` / `snapshot_dir` 冲突。
  - `LocalDiskChunkStore::Initialize()` 负责创建目录、清理 staging 和重建本地 chunk index；app 只做装配和错误透传。
- 错误诊断
  - 参数错误返回 `kInvalidArgument`。
  - 配置解析和 override 冲突返回 `kConfigError`。
  - identity 冲突、损坏、durability 问题返回 `kIdentityError` 或 `kUnsupported`。
  - chunk store 初始化或 gRPC 绑定失败返回 `kStartupError`。

## 4. 是否保持 StorageNode data-plane 与 metadata control-plane 边界

- 是。
- 本任务没有修改 chunk 写入、读取、删除、checksum、durable publish、restart recovery 语义。
- 没有实现 ViewNode registration / heartbeat loop、placement、metadata manifest authority、Raft quorum、leader election 或 commit 逻辑。
- app 只负责启动装配、参数解析、身份校验、错误展示和服务生命周期管理。

## 5. 是否发现不合理点 / 警告 / 风险

- `StorageNode` 配置仍允许 `node_id` 为空，因此当 cluster config 中存在多个未命名 `StorageNode` 时，`storage_node_app` 不能凭空推断当前节点身份；当前实现只允许“单个未命名条目 + 显式 `--node_id`”的受控启动路径。
- `--listen` / `--data_dir` override 目前被严格视为本地测试用途；如果 override 引入与其它角色节点的 endpoint/path 冲突，会直接拒绝启动，避免把本地测试参数误当成新的 authority。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `risk-register.md`。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- apps/storage_node_app.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md modules/cluster/cluster_config.h modules/cluster/cluster_config.cpp modules/cluster/node_identity.h modules/cluster/node_identity.cpp modules/store/node/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t047-storage-node-app-startup.md
```

结果：已执行。输出中包含本任务新增/修改的 `apps/storage_node_app.cpp`、`tasks.md`、任务报告，也包含工作区里已存在的 `modules/cluster/cluster_config.*` 与 `tasks.md` 上 T046 差异。经复核，本任务未继续修改 `app-cli.md`、`modules/cluster/*`、`modules/store/node/module-notes.md`。

### 最小 target 构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_node_app'
```

结果：PASS。

### 告警处理

- 首次构建出现一个 `[[nodiscard]]` 误用于 `void` helper 的编译告警。
- 已移除该属性后重新执行：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --build --preset debug-ninja-safe --target storage_node_app'
```

结果：PASS。
