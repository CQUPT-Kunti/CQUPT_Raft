# T041 Start Peer Sync Loop And Retry Backoff From View Node App

## 做了什么

在 `apps/view_node_app.cpp` 中把 ViewNode peer sync runtime wiring 接入了 app 生命周期：

- 启动时从 `cluster_config` 读取当前 ViewNode 的 `peer_seed_endpoints`
- 基于现有 `PullPeerViewSnapshot` / `PushPeerViewSnapshot` RPC 和
  `ExportPeerSnapshot` / `ImportPeerSnapshot` registry API 启动后台 peer sync loop
- peer 不可达或 sync 失败时做 retry / exponential backoff
- shutdown 时安全停止 peer sync 线程，不阻塞主服务退出

本任务没有修改 Raft membership、StorageNode dynamic join、Metadata learner join，也没有改 merge ordering 语义。

## 修改文件

- `apps/view_node_app.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t041-start-peer-sync-loop-and-retry-backoff-from-view-node-app.md`

## peer sync loop 如何启动、重试和停止

### 启动

- `ResolveStartupConfig(...)` 现在会把 `ResolvedClusterNodeConfig.view_peer_seed_endpoints` 带入 `ViewNodeStartupConfig`
- app 启动成功、gRPC server bind 成功后，构造 `PeerSyncTarget` 列表
- 每个 target 持有：
  - peer endpoint
  - `ViewNodeClient`
  - 连续失败次数
  - 下一次允许尝试的时间点

### 同步行为

每轮对单个 peer 执行：

1. `PullPeerViewSnapshot(...)` 拉取对端 observed registry snapshot
2. 本地 `ImportPeerSnapshot(...)` 导入对端 observed state
3. 本地 `ExportPeerSnapshot(...)` 导出当前 registry snapshot
4. `PushPeerViewSnapshot(...)` 把本地 snapshot 推给对端

这样既能导入 peer 状态，也能把本地状态传播出去。

### retry / backoff

- peer sync loop 在独立后台线程运行，不阻塞主服务启动和请求处理
- 失败时按连续失败次数做指数退避
- backoff 上限由 app 内部 cap 控制，不会无限放大
- transport failure、RPC summary failure、import/export failure 都会记录诊断并触发下一轮 backoff
- 恢复成功后会清零失败计数，并恢复到正常 `peer_sync_interval`

### 停止

- 复用现有 `g_stop_requested` stop flag
- peer sync 线程与 self refresh 线程一样在 shutdown 时 join
- gRPC client 调用本身带 timeout，不会形成无限等待线程

## 如何保证它只是 observed-state sync，不是 membership authority

- peer sync 只调用：
  - `PullPeerViewSnapshot`
  - `PushPeerViewSnapshot`
  - `ExportPeerSnapshot`
  - `ImportPeerSnapshot`
- 这些 API 只同步 `observed registry state`
- 没有触碰：
  - `initial_raft_membership`
  - Metadata voter / learner membership
  - Raft quorum
  - Storage manifest / Raft log
- app 层只是编排现有 observed-state contract，不做 membership 决策

## 新增或更新了哪些测试

没有新增测试文件。

原因：

- 当前仓库已有 `ViewNodeDiscovery` 覆盖 peer sync RPC、snapshot import/export、old-incarnation rejection、failover discovery 等关键行为
- 本任务主要是 app runtime wiring，当前没有合适的轻量 app harness 去做线程级单测
- 因此本任务以 targeted build + 既有 `ViewNodeDiscovery` 回归为主

## 验证命令

构建：

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target view_node_app test_view_node_discovery > tmp/test-logs/t041-build.log 2>&1
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure > tmp/test-logs/t041-ctest.log 2>&1
```

## 结果

- PASS
- `view_node_app` 定向构建通过
- `ViewNodeDiscovery` 28/28 测试通过
- local RPC startup/shutdown smoke：本任务未执行，SKIPPED
- Linux：已验证构建和测试
- Windows/macOS：未实测，pending
- 我已把 T041 勾选完成；`tasks.md` 中同时存在先前工作树留下的 T042/T043 勾选变更，本任务未回退这些既有改动

## 结论

- 可以进入后续任务，优先 T042 或后续 phase-05 收口任务
