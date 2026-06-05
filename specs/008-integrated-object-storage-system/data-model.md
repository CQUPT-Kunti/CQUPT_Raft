# Data Model: Integrated Object Storage System

**Feature**: 008-integrated-object-storage-system  
**Date**: 2026-06-05

## ClusterConfig

**Purpose**: 描述一次集群启动的目标拓扑和运行参数。

**Fields**:

- `cluster_id`: 集群标识，用于隔离 data_dir 和观测信息。
- `view_nodes`: ViewNode 列表，包含 endpoint、data_dir、可选 node_id、heartbeat policy。
- `metadata_nodes`: Raft MetadataNode 列表，包含 endpoint、data_dir、snapshot_dir、node_id、raft_id、voter/learner 初始角色。
- `storage_nodes`: StorageNode 列表或模板，包含 endpoint、data_dir、capacity_bytes、failure domain、可选 node_id。
- `initial_raft_membership`: 初始 voter/learner 配置，第一阶段由配置生成器生成。
- `chunk_policy`: chunk_size、replica_count、minimum_successful_writes、checksum algorithm。
- `timeouts`: discovery、metadata RPC、storage RPC、heartbeat、registration、commit deadline。

**Validation Rules**:

- endpoint 不得重复。
- MetadataNode 初始 voter 数必须为配置声明值，quorum 由 voter 总数计算。
- StorageNode capacity 必须大于 0。
- data_dir 必须按平台合法，且不能与其他节点冲突，除非明确用于同一节点重启。
- 配置生成结果必须可重复加载，不能依赖代码常量补足节点数量。

## NodeIdentity

**Purpose**: 节点本地持久身份，保障重启后身份稳定。

**Fields**:

- `node_id`: 系统分配或配置生成的稳定 ID。
- `node_type`: `view`、`meta`、`store`。
- `raft_id`: 仅 MetadataNode 需要，作为 Raft membership 使用的稳定 ID。
- `cluster_id`: 所属集群。
- `created_at_unix_ms`: 首次创建时间。
- `identity_version`: identity 文件格式版本。
- `source`: `config_generator`、`view_node_allocator`、`explicit_override`。

**Validation Rules**:

- node_id 非空，只允许安全字符。
- node_type 必须与启动角色一致。
- MetadataNode 必须有 raft_id，StorageNode 不得伪造 raft_id。
- 已存在 identity 与配置冲突时必须失败并报告，不得静默覆盖。

**Durability Rules**:

- 写入流程必须是临时文件 -> flush -> atomic publish -> directory durability 或明确平台 fallback。
- required durability operation 不得 no-op success。

## NodeRegistration

**Purpose**: ViewNode 中的节点观测记录。

**Fields**:

- `node_id`
- `node_type`
- `endpoint`
- `data_plane_endpoint`
- `control_plane_endpoint`
- `capacity`
- `health`
- `load`
- `failure_domain`
- `last_heartbeat_unix_ms`
- `liveness`: `LIVE`、`STALE`、`SUSPECT`、`DEAD`
- `metadata_status`: MetadataNode 专用，`REGISTERED`、`JOINING`、`LEARNER`、`VOTER`、`DOWN`
- `leader_hint`: 可选 MetadataNode leader 观测信息

**Validation Rules**:

- 注册到 ViewNode 只代表观测存在，不代表 Raft membership。
- 同一 node_id 重复注册必须幂等或报告冲突。
- endpoint 被不同 node_id 占用时必须报告冲突。

## RaftMembership

**Purpose**: Raft 自身已提交的成员配置，是 quorum 和 voter 身份的唯一权威。

**Fields**:

- `membership_epoch`
- `voters`: raft_id 列表
- `learners`: raft_id 列表
- `committed_log_index`
- `committed_term`

**Validation Rules**:

- quorum = floor(voters.size / 2) + 1。
- ViewNode 不得直接写入或覆盖此实体。
- 第一阶段 membership 由初始配置生成，运行时变更仅保留接口边界。

## WritePlan

**Purpose**: MetadataNode 生成的对象写入计划，指导 Client 将真实 chunk 写入 StorageNode。

**Fields**:

- `request_id`
- `bucket`
- `object_key`
- `object_id`
- `version`
- `object_size`
- `object_checksum`
- `state`: `PLANNED`、`PENDING`、`EXPIRED`、`COMMITTED`、`ABORTED`
- `chunk_layout`: chunk_index、offset、expected_size、expected_checksum。
- `placement`: 每个 chunk 的 StorageNode candidate/replica 分配。
- `expires_at_unix_ms`
- `created_at_unix_ms`

**Validation Rules**:

- WritePlan 不包含 chunk payload。
- 所有 placement 节点必须来自健康且容量满足的 StorageNode 集合。
- PENDING 未 commit 对象不得对普通下载可见。

## Placement

**Purpose**: 将 chunk replica 分配到 StorageNode。

**Fields**:

- `chunk_id`
- `required_replica_count`
- `minimum_successful_writes`
- `assigned_nodes`
- `excluded_nodes`
- `decision_reasons`
- `decision_epoch`

**Validation Rules**:

- assigned_nodes 数量必须满足 replica policy。
- excluded_nodes 必须记录排除原因，例如 dead、stale、capacity insufficient、overloaded、draining。
- 选择逻辑必须确定性可测试。

## ChunkManifest

**Purpose**: 已提交对象中每个 chunk 的强一致元数据。

**Fields**:

- `chunk_id`
- `chunk_index`
- `offset`
- `size`
- `checksum`
- `replica_nodes`
- `storage_state`

**Validation Rules**:

- checksum 必须来自 StorageNode 写入结果或 Client 预期值校验后的结果。
- replica_nodes 记录 node_id，不记录易变 endpoint。
- ChunkManifest 不包含 payload。

## ObjectManifest

**Purpose**: 已提交对象的可见元数据。

**Fields**:

- `bucket`
- `object_key`
- `object_id`
- `version`
- `size`
- `etag` 或 `object_checksum`
- `state`: `PENDING`、`COMMITTED`、`DELETED`
- `chunks`
- `create_time`
- `commit_time`
- `delete_time`

**State Transitions**:

```text
NONE -> PENDING -> COMMITTED -> DELETED
              \-> ABORTED/EXPIRED(cleanup-only)
```

**Validation Rules**:

- 只有 COMMITTED 对象能被普通下载读取。
- CommitObject 必须通过 Raft majority commit 生效。
- CommitObject 重试必须幂等或返回明确冲突。

## ObjectTransferSession

**Purpose**: Client 侧上传/下载执行上下文，不是 Raft 持久实体。

**Fields**:

- `request_id`
- `source_path` 或 `destination_path`
- `bucket`
- `object_key`
- `object_id`
- `version`
- `chunk_size`
- `concurrency`
- `bytes_completed`
- `checksum_state`
- `failure_summary`

**Validation Rules**:

- 不得把完整文件缓存为一个内存字符串。
- 每个 chunk 完成后记录可诊断状态。
- 下载最终必须校验对象 checksum。

## CleanupRecord

**Purpose**: 标记未提交、过期、失败或 orphan chunk 的清理候选。

**Fields**:

- `chunk_id`
- `object_id`
- `version`
- `node_id`
- `reason`
- `metadata_boundary`
- `created_at_unix_ms`
- `retry_count`
- `last_error`

**Validation Rules**:

- 清理不得删除已提交 manifest 仍引用的 chunk。
- 清理失败必须可重试并可诊断。
