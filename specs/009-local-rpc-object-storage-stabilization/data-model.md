# Data Model: Local RPC Object Storage Stabilization

## NodeIdentity

本地持久身份，保存在节点自己的 `identity_file` 路径。

Fields:

- `cluster_id`: 集群标识，必须与启动配置或 join 目标一致。
- `node_type`: `view`、`storage`、`metadata`。
- `node_id`: 长期逻辑身份，重启后复用。
- `raft_id`: 仅 MetadataNode 使用；StorageNode / ViewNode 不应携带。
- `membership_state`: Metadata dynamic join 可为 `joining`、`candidate`、`learner`、`voter`；Storage/View 不使用该字段作为 Raft authority。
- `created_at`: 首次创建时间。
- `persistent_generation`: 当前新格式 identity 的本地代际 / schema generation / diagnostics 字段，不承担旧格式兼容职责。
- `source`: config generator、local first-start、explicit local override、committed membership 等来源诊断。

Validation:

- Missing identity_file is valid for first start unless configuration requires pre-existing identity.
- 009 只支持当前 `NodeIdentity` 新格式；existing identity 缺少 `membership_state`、`persistent_generation` 或其他必填字段时必须 fail-fast。
- Existing legacy / old-format / unknown-format identity file must fail fast; no automatic upgrade, no silent field completion, and no auto-overwrite.
- Existing identity mismatch on cluster_id/node_type/node_id/raft_id must fail fast.
- Metadata bootstrap voter may create identity from bootstrap config.
- Metadata dynamic join may create candidate identity, but cannot become voter by local write alone.

## ProcessIncarnation

进程启动实例身份，每次进程启动生成。

Fields:

- `node_id`: 所属长期逻辑节点。
- `incarnation_id` or `boot_epoch`: 单调或唯一启动实例标识。
- `started_at_unix_ms`: 进程启动观测时间。
- `startup_sequence_base`: heartbeat sequence 初始基线。

Validation:

- A higher incarnation wins over lower incarnation during registry merge.
- Same incarnation uses heartbeat sequence ordering.
- Old incarnation updates must not override new incarnation even when observed_time is later.

## HeartbeatSequence

同一 incarnation 内的递增更新序号。

Fields:

- `incarnation_id`
- `sequence`
- `observed_time`
- `request_id`

Rules:

- Sequence must be greater than zero.
- Same incarnation + lower sequence is stale.
- Same incarnation + same sequence is idempotent.
- Same incarnation + higher sequence can apply only if it does not violate incarnation ordering.
- `observed_time` updates TTL and diagnostics; it is not global merge authority.

## ObservedNodeState

ViewNode registry 中的观测状态。

Fields:

- `cluster_id`
- `node_type`
- `node_id`
- `endpoint`
- `control_plane_endpoint`
- `data_plane_endpoint`
- `data_dir_fingerprint`
- `incarnation_id`
- `sequence`
- `registered_at_unix_ms`
- `last_seen_unix_ms`
- `observed_time`
- `liveness`
- `health`
- `capacity`
- `load`
- `failure_domain`
- `metadata_observation`

Validation:

- `node_id + node_type + endpoint + data_dir_fingerprint` conflicts must be diagnostic.
- ViewNode may display `membership_state`, but cannot make it authoritative.
- DEAD state from older incarnation cannot override LIVE state from newer incarnation.

## ViewRegistrySnapshot

ViewNode peer sync payload and diagnostic output.

Fields:

- `source_view_node_id`
- `source_incarnation_id`
- `snapshot_sequence`
- `generated_at_unix_ms`
- `observed_states[]`
- `warnings[]`

Merge rules:

1. Group by `cluster_id + node_id`.
2. Reject incompatible `node_type`, endpoint, or data_dir_fingerprint unless explicitly modeled as replacement.
3. Higher incarnation wins.
4. Same incarnation higher sequence wins.
5. `observed_time` updates TTL only after incarnation/sequence ordering allows the state.
6. Old snapshot cannot downgrade newer LIVE process state to DEAD.

## ViewPeer

Configured or discovered peer ViewNode seed.

Fields:

- `node_id`
- `endpoint`
- `identity_file`
- `peer_seed`
- `last_sync_sequence`
- `last_success_unix_ms`
- `last_error`

State transitions:

- `unknown -> reachable -> syncing -> degraded -> unavailable`
- Transport failure only affects peer sync diagnostics; it does not change Raft membership.

## StorageNodeRegistration

Discovery-only registration for data-plane nodes.

Fields:

- `cluster_id`
- `node_id`
- `incarnation_id`
- `endpoint`
- `chunk_data_dir`
- `capacity`
- `failure_domain`
- `health`
- `disk_pressure`
- `load`
- `sequence`

State transitions:

- `first_start -> identity_created -> rpc_started -> registered -> live`
- `live -> stale -> suspect/dead` by TTL when heartbeat stops
- `restart -> same node_id + new incarnation -> live`

Rules:

- StorageNode registration does not enter Raft log.
- New StorageNode can serve future object placement.
- Existing committed manifests are not modified by discovery changes.

## MetadataJoinCandidate

Dynamic MetadataNode before committed membership accepts it.

Fields:

- `cluster_id`
- `node_id`
- optional `raft_id`
- `client_endpoint`
- `raft_peer_endpoint`
- `identity_file`
- `join_token` optional for local dev
- `state`: `joining` / `candidate`

Validation:

- Candidate cannot vote.
- Candidate cannot become leader.
- Candidate cannot set local membership_state to voter without committed membership.

## LearnerMember

Metadata/Raft non-voter accepted through committed membership.

Fields:

- `node_id`
- `raft_id`
- `raft_peer_endpoint`
- `joined_at_log_index`
- `joined_at_term`
- `match_index`
- `applied_index`
- `commit_index`
- `snapshot_index`
- `catch_up_state`
- `ready_to_promote`
- `health`

State transitions:

- `candidate -> pending_add_learner -> learner`
- `learner -> catching_up`
- `catching_up -> ready_to_promote`
- `ready_to_promote -> waiting_for_pair` when single promote would make even voter count
- `ready_to_promote -> pending_batch_promote` when enough ready learners exist
- `pending_batch_promote -> voter` after committed membership change

Rules:

- Learner receives AppendEntries and InstallSnapshot.
- Learner is excluded from RequestVote and quorum.
- Learner cannot be elected leader.

## CommittedMembership

Raft membership authority.

Fields:

- `membership_epoch`
- `committed_log_index`
- `committed_term`
- `voters[]`
- `learners[]`
- `pending_change_id`

Invariants:

- `voters.size()` must be odd.
- Learners are not voters.
- Quorum is `floor(voters.size() / 2) + 1`.
- Every committed membership change must be represented in Raft log/config entry and applied consistently after restart.

## MembershipChangeBatch

Safe membership transition for odd-voter promote.

Fields:

- `change_id`
- `source_membership_epoch`
- `target_membership_epoch`
- `promote_learner_ids[]`
- `target_voters[]`
- `target_learners[]`
- `safety_mode`: `batched_membership_change` / `joint_consensus`
- `committed_log_index`
- `status`

Validation:

- Target voters count must be odd.
- No intermediate committed membership may contain even voter count.
- All promoted learners must be ready_to_promote before commit.
- Only one pending membership change is allowed unless batch/joint safety supports concurrency.
