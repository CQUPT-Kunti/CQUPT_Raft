# ViewNode 模块说明

## 模块职责

`modules/view/` 规划承载 008 阶段的 ViewNode 注册、心跳、服务发现和状态观测逻辑。它帮助 Client 发现 MetadataNode / StorageNode，帮助运维视角查看节点健康、容量、负载、leader hint 和冲突诊断。

ViewNode 的定位是 discovery-only / observation-only：它可以展示观测到的事实和候选端点，但不成为对象元数据、Raft membership 或 chunk 数据的权威。

## 核心概念

- `NodeRegistration`：记录 node_id、node_type、endpoint、data/control plane endpoint、capacity、health、load、failure_domain、last heartbeat 和冲突诊断字段。注册相同 node_id 且兼容 endpoint 时应幂等；同 endpoint 不同 node_id 或同 node_id 不兼容注册必须可诊断。
- `Heartbeat`：节点周期性上报 sequence、observed_at、health、load、capacity 和可选 leader hint。低 sequence 或旧观测不得覆盖较新事实。
- `Liveness`：ViewNode 根据 heartbeat 新鲜度推导 `LIVE`、`STALE`、`SUSPECT`、`DEAD` 等观测状态。状态转换是服务发现输入，不是 Raft quorum 或对象可见性的依据。
- `DiscoverMetadata`：返回可用 MetadataNode endpoint、leader hint、观测到的 membership 状态和 freshness。Client 仍必须处理 `NOT_LEADER`、quorum failure 和 MetadataNode 返回的权威结果。
- `DiscoverStorage`：返回 StorageNode endpoint、容量、负载和健康事实，供后续 placement / client 解析 endpoint 使用。placement 权威仍在 metadata control-plane 的策略和已提交 metadata 中。
- `GetClusterView`：返回 ViewNode、MetadataNode、StorageNode 的观测快照，包括 liveness、capacity、leader hint、冲突和 stale 警告。
- `leader hint`：MetadataNode leader 的非权威观测提示，用于减少 Client 重试成本；过期或错误时不得影响 Raft election 和 commit 安全。

## `view_registry.h` 接口边界

`modules/view/view_registry.h` 只定义 `viewdemo::ViewNodeRegistry` 的类型和接口边界，不包含注册、心跳排序、liveness transition 或 discovery snapshot 的实现。核心类型包括节点类型、liveness、健康状态、MetadataNode 观测角色、membership 观测状态、注册请求、heartbeat 请求、节点快照、diagnostic、metadata/storage discovery 结果和 cluster view 快照。

`ViewNodeRegistry` 对外暴露 `RegisterNode`、`HeartbeatNode`、`LookupNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView`、`size` 和 `config`。查询接口显式接收 `now_unix_ms`，方便后续 T016 使用确定性时间源实现和测试 liveness 计算。该头文件不包含 proto/gRPC 依赖；T018/T019 的 service adapter 负责 proto 字段与 registry 类型之间的映射。

## `view_service_impl.h` 接口边界

`modules/view/view_service_impl.h` 只定义 `view::ViewNodeService::Service` 到 `viewdemo::ViewNodeRegistry` 的 gRPC adapter 声明，不实现 RPC 逻辑。它应：

- 通过依赖注入持有 `ViewNodeRegistry`
- 为 `RegisterNode`、`HeartbeatNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView` 预留同步 unary RPC 处理边界
- 允许注入确定性时间源，便于后续 T019 和测试控制 `now_unix_ms`

它不负责：

- 修改 Raft membership 或缩小 quorum
- 决定对象 `COMMITTED` 可见性
- 读写 StorageNode chunk payload
- 承载 `view_node_app` 启动和进程生命周期编排

## `view_client.h` / `view_client.cpp` 边界

`modules/view/view_client.h` / `modules/view/view_client.cpp` 负责把调用方的 `RegisterNode`、`HeartbeatNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView` 请求映射到 `ViewNodeService` unary RPC，并把 proto response 转回本地 `viewdemo` 结果类型。当前实现支持：

- 按 RPC 类型应用默认 timeout，并允许单次调用覆盖 timeout / `wait_for_ready`
- 返回 transport 诊断（gRPC status code、message、details、effective timeout、retryable）
- 把 summary / snapshot / warning 转为本地结果与 diagnostics

它不负责：

- 对 leader hint 做强一致解释
- 决定对象 `COMMITTED` 可见性
- 修改 Raft membership 或 quorum
- 操作 StorageNode payload
- 实现 upload/download 编排或 app 注册/心跳循环

## `view_service_impl.cpp` 实现边界

`modules/view/view_service_impl.cpp` 负责把 `proto/view.proto` 请求映射到 `ViewNodeRegistry`，并把 registry 结果映射回 gRPC response。它应：

- 保持 `RegisterNode`、`HeartbeatNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView` 的同步 unary adapter 实现
- 返回可诊断的 summary、snapshot、warning 和 leader hint 观测信息
- 支持注入 `now_unix_ms`，避免 discovery / cluster view 实现偷偷依赖不可控全局时间
- 把 registry 异常收敛为明确的 gRPC internal failure，而不是静默吞掉

它不负责：

- 把注册结果解释为已提交 Raft membership
- 把 leader hint 当成强一致 leader authority
- 把 StorageNode discovery 结果解释为对象可见性
- 保存 object manifest 或 chunk payload

## `view_registry.cpp` 实现边界

`modules/view/view_registry.cpp` 实现内存 registry，不做持久化、RPC 适配或 app 启动逻辑。当前实现负责注册幂等检查、同 cluster endpoint 冲突诊断、heartbeat sequence 去旧、观测事实刷新、按 `stale_timeout` / `suspect_timeout` / `dead_timeout` 计算 `LIVE` / `STALE` / `SUSPECT` / `DEAD`，以及生成 MetadataNode、StorageNode 和 ClusterView 快照。

Discovery snapshot 只返回候选端点和观测事实。MetadataNode 的 leader hint 只按观测 term / 时间选择最新提示；StorageNode 的 capacity、health、load 只用于发现过滤和诊断，不代表对象可见性。后续测试应覆盖重复注册、endpoint 冲突、stale heartbeat、liveness 过滤、leader hint 选择和 storage writable 过滤。

## Non-Authority Boundary

ViewNode 不保存 object manifest 的一致性权威副本，不决定对象是否 `COMMITTED` 可见，不参与 `CommitObject`，不直接读写 StorageNode chunk 数据。

ViewNode 不修改 Raft membership，不降低 Raft quorum，不参与 Raft leader election，也不把新注册 MetadataNode 直接提升为 voter。MetadataNode 的 `REGISTERED`、`JOINING`、`LEARNER`、`VOTER`、`DOWN` 只是在 ViewNode 中展示的观测状态；Raft voter 身份和 quorum 只能来自 Raft 已提交 membership。

## 与其他组件的关系

- MetadataNode：向 ViewNode 注册 endpoint、raft_id、观测角色、membership 观测状态和 leader hint。对象 manifest、WritePlan、CommitObject、PENDING/COMMITTED 可见性仍由 Raft metadata control-plane 决定。
- StorageNode：向 ViewNode 注册 node_id、endpoint、capacity、health、load 和 failure domain，并持续 heartbeat。真实 chunk 写入、读取、publish durability 和清理由 StorageNode data-plane 负责。
- Client：通过 ViewNode 获取 MetadataNode / StorageNode 候选地址和集群状态，但上传下载必须以 MetadataNode 的 WritePlan / COMMITTED manifest 和 StorageNode chunk RPC 结果为准。
- ViewNode：多个 ViewNode 之间在第一阶段不提供共识；单个 ViewNode 的 registry 是发现和观测缓存，不是强一致元数据副本。

## 后续扩展点

- 多 ViewNode 启动和 Client failover。
- ViewNode 自身高可用或 registry 复制，但必须先定义一致性和故障语义。
- 注册租约、租约续期和明确的过期策略。
- registry 状态持久化及 Linux/Windows durability contract。
- 认证授权、租户隔离和管理审计。
- 更细粒度的容量、负载、failure domain、draining/maintenance 状态和诊断指标。

## 测试与诊断要求

后续实现应覆盖注册幂等、身份冲突、heartbeat sequence 去旧、liveness 超时转换、发现快照 freshness、leader hint 过期、StorageNode dead 排除和 MetadataNode 注册不改变 quorum 等场景。诊断输出应保留 request_id、node_id、endpoint、sequence、状态和冲突原因，便于高并发与故障恢复场景定位。
