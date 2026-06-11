# Contract: ViewNode Self Refresh And Peer Sync

## Scope

本合同定义 ViewNode 自身状态刷新、多 ViewNode observed registry 同步、liveness TTL、incarnation-aware merge 与重启恢复边界。ViewNode 仍然只负责 discovery / observation，不负责 Raft voter / learner membership。

## Baseline Entry Points

- 当前 ViewNode registry：`modules/view/view_registry.h`、`modules/view/view_registry.cpp`
- 当前 ViewNode RPC adapter/client：`modules/view/view_service_impl.*`、`modules/view/view_client.*`
- 当前 ViewNode app：`apps/view_node_app.cpp`
- 当前 ViewNode 测试：`tests/view_node_discovery_test.cpp`
- 当前报告确认问题：`view-1` 自身 registry 记录只注册一次，可能因无 self-heartbeat 变为 stale/dead。

## Self Refresh Requirements

- 如果 ViewNode 把自己纳入 cluster view，必须启动独立 self refresh loop。
- self refresh 不依赖 StorageNode 或 MetadataNode 心跳。
- 健康运行中的 ViewNode 运行超过 dead TTL 后，自己的状态仍为 `LIVE`。
- self state 至少包含 `cluster_id`、`node_type=view`、`node_id`、endpoint、incarnation、sequence、observed_time、health、liveness。
- 停止 self refresh 后，TTL 可以驱动状态转为 `STALE`、`SUSPECT`、`DEAD`。

## Peer Sync Requirements

- 009 至少支持 2 个 ViewNode active-active discovery。
- ViewNode 配置包含自身 endpoint、registry data dir、peer seed、self refresh interval、peer sync interval、liveness/suspect/dead timeout。
- ViewNode 之间可用 PushViewSnapshot、PullViewSnapshot、MergeObservedState 或等价 RPC 同步 observed registry。
- peer sync 是最终一致，不提供线性一致和 Raft membership 权威。
- 任一 ViewNode 故障时，客户端仍应能从另一个 ViewNode 获取 Metadata leader hint 与 LIVE StorageNode 候选。

## Merge Ordering

1. 按 `cluster_id + node_id` 定位同一逻辑节点。
2. `node_type`、endpoint、data_dir_fingerprint 冲突必须给出诊断，不得静默覆盖。
3. 更高 incarnation / boot epoch 优先。
4. 同一 incarnation 下更高 sequence 优先。
5. `observed_time` 只用于 TTL/liveness 判断，不得单独覆盖更高 incarnation 的状态。
6. 旧 incarnation 的 DEAD/STALE/SUSPECT 不得覆盖新 incarnation 的 LIVE。
7. 旧 registry snapshot 不得因为生成时间较新而覆盖新进程状态。

## Persistence And Recovery Boundary

- 009 必须在实现前明确 ViewNode registry 是否持久化，以及重启后如何恢复。
- 如果引入 registry snapshot，格式必须版本化并支持旧 incarnation 防护。
- 如果某阶段保留内存型 registry，必须在计划和任务报告中明确重启后由 self refresh、node heartbeat、peer sync 重新收敛的边界。

## Validation Requirements

- `tests/view_node_discovery_test.cpp` 覆盖单 ViewNode self refresh 超过 TTL 后仍 `LIVE`。
- 同一测试或新增 ViewNode peer sync 测试覆盖停 self refresh 后 TTL 转换。
- 双 ViewNode 测试覆盖 registry 同步、ViewNode failover、重启后新 incarnation 覆盖旧状态。
- 旧 incarnation heartbeat / snapshot 的 observed_time 即使更新，也不能覆盖新 incarnation。

