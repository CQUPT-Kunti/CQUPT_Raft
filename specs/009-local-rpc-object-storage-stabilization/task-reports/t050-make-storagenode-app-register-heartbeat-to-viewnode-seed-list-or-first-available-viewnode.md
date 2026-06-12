## T050

### 做了什么

- 收口 `apps/storage_node_app.cpp` 中的 StorageNode 到 ViewNode 的注册/heartbeat wiring。
- 保持现有“按 seed list 依次尝试，找到第一个可用 ViewNode 即注册”的流程，并继续在 heartbeat 失败后重新按 seed list failover。
- 把本地 `process incarnation` 明确接入到：
  - 本地 registry seed
  - 发往 ViewNode 的 heartbeat request
- 调整 discovery 观察映射：当 StorageNode 本地 facts 明确 `writable=false` 时，不再把它伪装成健康可写节点。

### 修改文件

- `apps/storage_node_app.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t050-make-storagenode-app-register-heartbeat-to-viewnode-seed-list-or-first-available-viewnode.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

### StorageNode app 现在如何连接 ViewNode seed list / first available ViewNode

- 启动时从 cluster config 收集 `view_nodes[*].endpoint` 作为 seed list。
- 启动成功绑定本地 Storage RPC 之后，调用 `RegisterWithAnyViewNode(...)`：
  - 从当前活跃 seed 开始尝试
  - 当前 seed 不可用时依次尝试后续 seed
  - 任一 seed 注册成功就记录为当前 active ViewNode
- 运行期 heartbeat 先发往当前 active ViewNode。
- 若 heartbeat 失败，则调用 `RegisterWithAnyViewNode(...)` 重新按 seed list 选择第一个可用 ViewNode，实现 failover 到其它 seed，而不是 silent success 或直接退出。
- 如果所有 seed 都不可用，app 会输出明确错误诊断；启动阶段会直接失败，运行期会持续按 heartbeat 周期重试注册。

### 注册和 heartbeat 是否接入本地 identity / incarnation

- 是。
- app 继续使用已验证的本地 `node.identity` 和 `CreateProcessIncarnation(...)` 结果。
- ViewNode register request 继续通过带有 `incarnation_id` 的 `request_id` 绑定本次进程实例。
- ViewNode heartbeat request 现在显式填写 `incarnation_id`。
- 本地 `StorageNodeRegistry` 启动 seed 记录也保存当前 `incarnation_id`，避免 app 内部观察状态与外发 heartbeat 边界脱节。

### 新增或更新了哪些测试

- 没有新增 app-level 测试文件。
- 原因：当前仓库没有直接覆盖 `storage_node_app` 的现成 gtest harness；本任务保持既有 `storage_heartbeat_registry` 回归通过，并通过 `storage_node_app` 定向构建验证 app wiring 未破坏现有依赖边界。

### 验证命令和结果

- 构建命令：
  - `(
    flock -n 9 || exit 99
    cmake --build --preset debug-ninja-low-parallel --target storage_node_app test_storage_heartbeat_registry
    ) 9>/tmp/cqupt_raft_build.lock`
  - 结果：PASS
- 测试命令：
  - `ctest --preset debug-tests -R "^storage_heartbeat_registry$" --output-on-failure`
  - 结果：PASS
- 测试日志：
  - `tmp/test-logs/t050-ctest.log`
- local RPC startup/status smoke：
  - 未执行
  - 原因：本任务优先做 app wiring 收口和 targeted build/test；app-level 运行期多进程 smoke 留给后续 local RPC 示例验证阶段或专门 smoke 任务。

### 结果

- 状态：PASS
- 已在 `tasks.md` 中只勾选 T050 完成。
- 可以进入 T051；本任务没有实现 placement policy、旧对象 rebalance、Raft membership 或 metadata authority 相关逻辑。
