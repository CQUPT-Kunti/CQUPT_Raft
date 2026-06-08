# T065 任务报告：storage_client status

## 1. 修改了哪些文件

- `apps/storage_client.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t065-storage-client-status.md`

## 2. storage_client status 做了什么

- 在 `storage_client` 中新增 `status` 子命令，并补齐命令行帮助输出。
- 调整参数解析边界，使 `status` 只要求 `--config`，并允许可选 `--request-id`；同时显式拒绝 `upload/download` 专用参数，避免命令语义混淆。
- 复用现有 `ViewNodeClient::GetClusterView(...)` 查询 ViewNode 的 cluster view，不新增写路径、不修改集群状态。
- 新增 status 输出格式：
  - 总览行：`request_id`、`cluster_id`、ViewNode 目标 endpoint、观测时间、各类节点数量
  - 顶层 `leader_hint`
  - `non_authority_boundary` 提示行
  - `view_node` / `metadata_node` / `storage_node` 逐节点观测行
  - `diagnostic` 诊断行
- 为 `status` 增加 ViewNode cluster view timeout 配置复用，沿用现有 discovery timeout，不改 upload/download 传输逻辑。

## 3. status 如何通过 ViewNode cluster view 展示观测信息

- `status` 通过 config 中的 `cluster_id` 和 ViewNode endpoint 创建 `ViewNodeClient`。
- 调用 `GetClusterView` 时设置：
  - `include_dead_nodes=true`
  - `include_warnings=true`
- 输出内容按 cluster view snapshot 展示：
  - `ViewNode`
    - `node_id`
    - `endpoint`
    - `liveness`
    - `health`
    - `disk_pressure`
    - `last_seen_unix_ms`
    - `last_sequence`
  - `MetadataNode`
    - 上述公共观测字段
    - `raft_id`
    - `raft_role`
    - `membership_observation`
    - `observed_term`
    - `commit_index`
    - `membership_epoch`
    - 节点级 `leader_hint`
  - `StorageNode`
    - 上述公共观测字段
    - `total_capacity_bytes`
    - `used_capacity_bytes`
    - `available_capacity_bytes`
    - `chunk_count`
    - `active_reads`
    - `active_writes`
    - `queued_ops`
    - `write_admission_overloaded`
    - `read_admission_overloaded`
    - `zone/rack`
- 顶层 cluster view 自带 `leader_hint` 时会单独输出，便于人工诊断 leader 观测情况。
- ViewNode 返回的 warning / conflict / stale 等诊断通过 `diagnostic` 行打印，不输出 raw payload、chunk bytes 或完整内部日志。

## 4. 是否保持 status / ViewNode non-authority 边界

- 是。
- 本任务只读调用 `GetClusterView`，没有新增任何写 RPC。
- 没有修改 Raft membership、quorum、commit、leader election 语义。
- 输出中显式增加 `non_authority_boundary` 行，说明：
  - `membership_observation_source=view_node`
  - `raft_membership_authority=false`
  - `object_manifest_authority=false`
- `status` 展示的 membership / leader hint 仅是 ViewNode 的 observation，不会被解释为已提交 membership 或 object manifest 权威。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `storage_client` 仍通过轻量文本/JSON 抽取方式读取 config 中的 `cluster_id` 和 ViewNode endpoint，没有切换到统一 `ClusterConfig` 反序列化入口；本任务保持现状以避免扩大既有 upload/download 配置行为变更。
- 在没有启动 ViewNode 的场景下，`status` 会返回明确的 gRPC 连接失败诊断；这符合 discovery-only 查询边界，但也说明后续 smoke/E2E 若要看到完整 cluster view，仍需 T061/T062 把 registration / heartbeat loop 接起来。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `risk-register.md`。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- apps/storage_client.cpp modules/view/view_client.h modules/view/view_client.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t065-storage-client-status.md
```

结果：已执行。此次任务实际改动集中在 `apps/storage_client.cpp`、`tasks.md` 中 T065 从 `[ ]` 到 `[X]` 的勾选以及本任务报告；`tasks.md` 的 diff 中还能看到工作区原本已存在的其它任务状态变更。经复核，本任务未修改 `modules/view/view_client.h`、`modules/view/view_client.cpp`、`app-cli.md`。

### 最小 target 构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_client' \
  || echo "build lock busy, skip storage_client build in this window"
```

结果：PASS。

### 最小 CLI smoke test

先生成临时 config，再直接执行 `status`：

```bash
./build/linux/safe/storage_client generate-config --out /tmp/.../status-config.json --base-dir /tmp/.../cluster
./build/linux/safe/storage_client status --config /tmp/.../status-config.json
```

结果：

- `generate-config` 返回 `0`
- `status` 返回 `4`
- stderr 输出清晰诊断：
  - `status FAILED`
  - `target_endpoint=127.0.0.1:7001`
  - `grpc_code=14`
  - `retryable=true`
  - `message=failed to connect to all addresses ... Connection refused`

说明：在未启动 ViewNode 服务时，`status` 能正确进入 cluster view 查询路径，并返回明确的 RPC 失败诊断，而不是误走 upload/download 分支。
