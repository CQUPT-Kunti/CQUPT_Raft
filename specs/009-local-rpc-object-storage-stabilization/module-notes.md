# Module Notes: 009 Local RPC Object Storage Stabilization

## `modules/cluster`

Responsibility: local identity lifecycle and cluster config parsing.

Inputs: `identity_file`, `cluster_id`, node role, optional Metadata `raft_id`, bootstrap/dynamic join mode.

Outputs: validated persistent `NodeIdentity`, process incarnation seed, diagnostics for mismatch/corruption.

Easy misuse: treating missing `identity_file` as startup error, or treating local Metadata identity state as proof of committed voter membership.

## `modules/view`

Responsibility: observed registry, liveness TTL, ViewNode self state, ViewNode peer sync, discovery output.

Inputs: Register/heartbeat requests, self refresh ticks, peer registry snapshots, local time source.

Outputs: cluster view, liveness state, peer sync diagnostics.

State transitions: LIVE -> STALE -> SUSPECT -> DEAD when refresh stops; restart creates new incarnation and old incarnation updates are rejected.

Easy misuse: using `observed_time` as merge authority, or using ViewNode records to decide Raft membership.

## `modules/store/node`

Responsibility: StorageNode service registration, heartbeat, capacity/load/health reporting.

Inputs: storage identity, process incarnation, heartbeat interval, capacity and disk pressure probes.

Outputs: ViewNode registration/heartbeat payloads and local service status.

Easy misuse: assuming StorageNode join affects old object manifests or Raft quorum.

## `modules/store/placement` And `modules/store/transfer`

Responsibility: choose LIVE StorageNode candidates for future writes and move object chunks through existing data-plane RPC.

Inputs: Metadata write plan request, ViewNode-discovered LIVE StorageNode candidates, capacity/failure-domain hints.

Outputs: write plan and object transfer operations.

Easy misuse: requiring dynamic StorageNode to rebalance old objects in 009, or using stale discovery records without liveness filtering.

## `modules/raft/service`

Responsibility: Metadata authority APIs, leader redirection, dynamic Metadata join authority endpoint.

Inputs: Metadata client requests, JoinMetadataCluster request, current leader/term, committed membership state.

Outputs: metadata results, `NOT_LEADER`/retry hints, AddLearner/promote proposal outcomes.

Easy misuse: accepting join based only on ViewNode observation or local identity without committed Raft config log.

## `modules/raft/node`

Responsibility: Raft state, committed membership, quorum/election rules, learner/voter transitions.

Inputs: committed config log entries, AppendEntries/RequestVote/InstallSnapshot, membership change proposals.

Outputs: applied membership, quorum summary, learner progress and promotion status.

State transitions: candidate dynamic MetadataNode -> learner -> ready_to_promote -> waiting_for_pair or pending_batch_promote -> voter after commit.

Easy misuse: counting learners in quorum, allowing learners to vote/lead, or committing even voter counts.

## `modules/raft/replication`

Responsibility: replicate log and snapshot to followers and learners, track match/applied progress.

Inputs: follower/learner peer set, log entries, snapshot availability, network responses.

Outputs: match_index/applied_index, catch-up state, ready-to-promote signal.

Easy misuse: treating learner replication as idle waiting while blocked by odd-voter promote; learners must continue catch-up.

## `apps`

Responsibility: parse config, load/create identity, start RPC servers, wire heartbeat/self refresh/join clients.

Inputs: local config files, endpoint flags, identity paths, ViewNode seed lists.

Outputs: running node process and logs.

Easy misuse: embedding complex Raft membership logic in app files instead of delegating to modules.

