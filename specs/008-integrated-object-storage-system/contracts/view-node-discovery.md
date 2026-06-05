# Contract: ViewNode Discovery And Registration

**Purpose**: 定义 ViewNode 作为节点注册、服务发现和状态观测组件的外部契约边界。

## Responsibilities

- 接收 ViewNode、MetadataNode、StorageNode 注册。
- 接收周期性 heartbeat、capacity、health、load 和 role 状态上报。
- 向 Client 和管理工具返回可用 MetadataNode 地址、StorageNode 地址、leader hint 和节点健康信息。
- 展示 Raft 节点观测状态，但不作为 Raft membership authority。

## Non-Authority Boundary

- 注册到 ViewNode 不等于加入 Raft membership。
- ViewNode 不得把 Raft 节点直接变为 voter。
- ViewNode 不得修改 Raft quorum、commit 规则或 election 规则。
- ViewNode 不得保存对象 manifest 的权威副本。

## Logical Operations

### RegisterNode

**Request**:

- `request_id`
- `cluster_id`
- `node_id`
- `node_type`: `view`、`meta`、`store`
- `endpoint`
- `data_dir_fingerprint`
- `capacity` for StorageNode
- `raft_id` and `raft_observed_role` for MetadataNode
- `metadata_membership_observed_state`: `REGISTERED`、`JOINING`、`LEARNER`、`VOTER`、`DOWN`

**Response**:

- status code
- `created`
- `idempotent`
- accepted node snapshot
- diagnostic message

**Rules**:

- Same node_id + compatible endpoint is idempotent.
- Same endpoint + different node_id is conflict unless explicit replacement mode is configured.
- MetadataNode registration is observational only.

### HeartbeatNode

**Request**:

- `request_id`
- `node_id`
- `sequence`
- `observed_at_unix_ms`
- `health`
- `capacity`
- `load`
- optional `leader_hint`

**Response**:

- status code
- accepted sequence
- `stale_ignored`
- current node snapshot

**Rules**:

- Lower sequence or older observation must be ignored as stale.
- Heartbeat timeout transitions node liveness from LIVE to STALE/SUSPECT/DEAD.

### DiscoverMetadata

**Request**:

- `request_id`
- `cluster_id`
- optional `prefer_leader`

**Response**:

- live MetadataNode endpoints
- best known leader hint
- observed membership summary
- freshness timestamp

**Rules**:

- Returned nodes are candidates for Client metadata RPC.
- If leader hint is stale, Client must still handle NOT_LEADER redirect.

### DiscoverStorage

**Request**:

- `request_id`
- `cluster_id`
- optional filters: health, capacity, zone, rack

**Response**:

- live StorageNode endpoint snapshots
- capacity and load facts
- freshness timestamp

**Rules**:

- Placement authority remains MetadataNode/placement policy, not Client.
- Client may use StorageNode endpoint from manifest node_id resolution for reads.

### GetClusterView

**Request**:

- `request_id`
- `cluster_id`

**Response**:

- view nodes
- metadata nodes
- storage nodes
- leader observation
- warnings about stale or conflicting identity records

## Failure Semantics

- ViewNode unavailable: new discovery fails or uses another configured ViewNode; already obtained manifest-based reads do not require ViewNode.
- Stale records: returned with liveness state and excluded from healthy placement.
- Duplicate identity: conflict response with both observed endpoints.
