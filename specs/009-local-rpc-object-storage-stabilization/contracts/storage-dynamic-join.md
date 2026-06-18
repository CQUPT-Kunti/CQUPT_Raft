# Contract: StorageNode Dynamic Join

## Scope

本合同定义 StorageNode 运行中动态注册、heartbeat、discovery 可见性和后续对象 placement 可用性。StorageNode join 是服务发现问题，不是 Raft membership 变更。

## Baseline Entry Points

- 当前 StorageNode app：`apps/storage_node_app.cpp`
- 当前 StorageNode registry/service/client：`modules/store/node/storage_node_registry.*`、`modules/store/node/storage_node_service.*`、`modules/store/node/storage_node_client.*`
- 当前 placement / transfer：`modules/store/placement/placement_manager.*`、`modules/store/transfer/object_transfer.cpp`
- 当前测试：`tests/storage_heartbeat_registry_test.cpp`、`tests/storage_node_service_test.cpp`、`tests/storage_node_client_test.cpp`、`tests/integrated_object_storage_e2e_test.cpp`
- 当前 example：`examples/object-storage-local-3meta-6store`

## Configuration Boundary

StorageNode 配置只包含：

- `cluster_id`
- `role=storage`
- `identity_file`
- RPC listen / advertise endpoint
- chunk data dir
- capacity / failure domain / health report 基础信息
- ViewNode seed 地址
- heartbeat interval / timeout
- 本地开发可选 join token / allowlist

StorageNode 不要求配置完整集群拓扑。

## Join Flow

1. StorageNode 启动。
2. 如果 `identity_file` 不存在，本地创建 storage identity。
3. 本次启动生成新的 incarnation / boot epoch。
4. 启动 Storage RPC。
5. 向一个或多个 ViewNode 注册；早期实现可先向第一个可用 ViewNode 注册，再通过 peer sync 扩散。
6. 持续 heartbeat，携带 node_id、incarnation、sequence、endpoint、capacity、load、health、disk pressure、writable status。
7. ViewNode 纳入 discovery registry。
8. 后续 CreateWritePlan / placement 可看到该 LIVE StorageNode。

## Invariants

- StorageNode 动态注册不进入 Raft log。
- StorageNode 动态注册不改变 Raft quorum、election、metadata committed manifest。
- 新 StorageNode 可参与后续新对象 placement。
- 009 不要求旧对象自动 rebalance 到新 StorageNode。
- StorageNode 退出、失联、重启后，ViewNode 只能更新 discovery/observation 状态，不能修改已提交对象 manifest。

## Validation Requirements

- `tests/storage_heartbeat_registry_test.cpp` 覆盖注册、heartbeat、TTL、重复注册、旧 sequence / incarnation 防护。
- `tests/integrated_object_storage_e2e_test.cpp` 或新增 integration test 覆盖集群运行中新增 StorageNode，再上传新对象。
- local RPC example 必须验证运行中加入，不是静态配置后一次性启动。
- 验证建议优先构建 `storage_node_app`、`storage_client`、`integrated_object_storage_e2e` 相关 target。

