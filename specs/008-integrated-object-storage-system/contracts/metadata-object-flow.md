# Contract: Metadata Object Flow

**Purpose**: 定义 Client 与 Raft MetadataNode 之间的对象 metadata/control-plane 行为。

## Authority

- Raft MetadataNode 是 object manifest、object state、version、commit visibility 和 Raft membership 的一致性权威。
- Raft log/snapshot 只保存 metadata，不保存真实 payload。
- CommitObject 只有通过 Raft majority commit 后才可见。

## Logical Operations

### CreateWritePlan

**Input**:

- `request_id`
- `bucket`
- `object_key`
- optional `object_id`
- expected object size
- expected object checksum
- chunk size
- replica policy
- client timestamp

**Output**:

- status
- object_id
- version
- PENDING object record
- chunk layout
- placement per chunk
- write token or idempotency key
- leader hint if not leader

**Rules**:

- Must be processed by current leader or return NOT_LEADER with leader hint.
- Must reject if no healthy/capacity-sufficient StorageNode set can satisfy placement.
- Must not include payload.

### CommitObject

**Input**:

- `request_id`
- `bucket`
- `object_key`
- `object_id`
- `version`
- size
- object checksum
- chunk write results: chunk_id, chunk_index, offset, size, checksum, replica node_id list

**Output**:

- status
- committed ObjectManifest on success
- leader hint on NOT_LEADER
- conflict detail on idempotency/state conflict

**Rules**:

- Must transition PENDING -> COMMITTED only after Raft commit.
- Must reject empty chunk manifest for non-empty object.
- Must be idempotent for replayed request_id with identical payload.
- Must detect conflicting replay with same request_id and different manifest.

### AbortObject

**Input**:

- `request_id`
- object identity
- reason

**Output**:

- status
- object state
- cleanup candidates if available

**Rules**:

- Aborted or expired objects are not visible for reads.
- Cleanup must not delete committed chunks.

### HeadObject / GetObjectManifest

**Input**:

- bucket
- object_key
- optional object_id/version

**Output**:

- found flag
- ObjectManifest if COMMITTED
- status and leader/freshness diagnostics

**Rules**:

- PENDING objects are hidden from normal read path.
- If strict linearizable read is not implemented, risk must be documented and tests must use leader-aware path.

## Quorum Rules

- quorum = floor(committed_voter_count / 2) + 1.
- The number of currently live nodes never reduces quorum.
- ViewNode registration never changes quorum.

## Payload Boundary

- Object bytes, chunk bytes, and complete file buffers are forbidden in metadata commands, Raft logs, Raft snapshots, and metadata snapshots.
