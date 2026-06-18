# Contract: Identity Lifecycle

## Scope

本合同定义 009 阶段三类节点的 `identity_file`、长期 `node_id`、Metadata `raft_id`、进程 `incarnation / boot epoch`、heartbeat `sequence` 与 `observed_time` 职责边界。它约束 `modules/cluster/node_identity.*`、`apps/view_node_app.cpp`、`apps/storage_node_app.cpp`、`apps/metadata_node_app.cpp` 以及相关配置解析路径。

## Baseline Entry Points

- 当前 identity 实现：`modules/cluster/node_identity.h`、`modules/cluster/node_identity.cpp`
- 当前 identity 测试：`tests/node_identity_test.cpp`
- 当前配置实现：`modules/cluster/cluster_config.h`、`modules/cluster/cluster_config.cpp`
- 当前配置测试：`tests/cluster_config_test.cpp`
- 当前 app 启动入口：`apps/view_node_app.cpp`、`apps/storage_node_app.cpp`、`apps/metadata_node_app.cpp`

## Required Semantics

- `identity_file` 是节点自己的本地持久身份文件路径。
- `identity_file` 首次不存在是正常情况，除非配置显式要求必须预置身份。
- 009 只支持当前 `NodeIdentity` 新格式；不存在 legacy compatibility、自动升级或静默补字段。
- 首次启动应创建 identity，原子写入后再启动 RPC 和注册流程。
- 重启必须复用长期 `node_id`，并生成新的 process incarnation / boot epoch。
- `node_id` 是长期逻辑身份；`incarnation / boot epoch` 是单次进程启动身份；`sequence` 只在同一 incarnation 内递增；`observed_time` 只服务 TTL/liveness 与诊断。
- StorageNode / ViewNode 的 `node_id` 可本地生成，不需要 ViewNode 分配。
- ViewNode 不是全局 ID authority。
- Metadata bootstrap voter 可以从 bootstrap 配置创建固定 `node_id` / `raft_id` / voter identity。
- Metadata dynamic join 节点只能创建 joining/candidate identity，不能通过本地文件让自己成为 voter。
- Metadata `membership_state` 从 joining 到 learner 再到 voter 的更新必须与 committed Raft membership 一致。

## First Start Flow

1. 节点启动并读取配置中的 `cluster_id`、`node_type`、`identity_file`。
2. 检查 `identity_file` 是否存在。
3. 如果不存在，根据节点类型创建本地身份。
4. 使用原子 publish 写入 identity。
5. 生成本次进程的 `incarnation / boot epoch`。
6. 启动 RPC。
7. StorageNode / ViewNode 进入 discovery 或 self observation；Metadata dynamic join 进入 leader discovery 和 join。

## Restart Flow

1. 节点启动并加载已有 `identity_file`。
2. 校验 `cluster_id`、`node_type`、`node_id`、Metadata `raft_id` / `membership_state` 与配置兼容。
3. 复用长期 `node_id`。
4. 生成新的 `incarnation / boot epoch`。
5. heartbeat `sequence` 从新 incarnation 的起点开始递增。
6. 旧 incarnation 的 registry 状态和 heartbeat 不得覆盖新 incarnation。

## Validation Requirements

- `tests/node_identity_test.cpp` 必须覆盖首次创建、重启复用、cluster_id mismatch、node_type mismatch、损坏文件 fail-fast。
- existing `identity_file` 若为 old-format / unknown-format / missing required new-format fields，必须 fail-fast，且不能当作 missing identity 重新创建。
- existing `identity_file` 出现 corrupt / mismatch 时，不能自动覆盖，也不能静默补 `membership_state` / `persistent_generation`。
- Metadata bootstrap voter 身份与 dynamic join candidate 身份必须分开测试。
- ViewNode / StorageNode 重启必须验证同一 `node_id` + 新 incarnation。
- 任何平台相关 atomic publish / durability 行为不能 silent no-op success；非等价平台行为必须记录明确错误或较弱保证。
