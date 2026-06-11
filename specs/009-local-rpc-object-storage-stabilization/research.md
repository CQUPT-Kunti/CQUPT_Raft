# Research: Local RPC Object Storage Stabilization

## Decision: ViewNode Self Refresh Is A First-Class Lifecycle Loop

**Rationale**: `apps/view_node_app.cpp` currently registers the local ViewNode into its in-memory registry once during startup and then only runs the gRPC server loop. `ViewNodeRegistry` computes liveness from `last_seen_unix_ms` and configured TTL, so a healthy ViewNode can later report itself as `STALE`, `SUSPECT`, or `DEAD`. The 009 report explicitly observes this as the remaining local RPC stabilization issue.

**Alternatives considered**:

- Treat self-stale as harmless status noise: rejected because clients and tests consume cluster view for discovery and diagnostics.
- Exempt ViewNode records from TTL: rejected because stopped ViewNodes would never become stale/dead.
- Require external heartbeats for ViewNode: rejected because self-liveness must not depend on StorageNode or MetadataNode activity.

## Decision: ViewNode Peer Sync Is Eventually Consistent Observed-State Merge

**Rationale**: ViewNode is explicitly discovery-only / observation-only. It must not become a Raft membership authority or a linearizable configuration service. Active-active peer sync is enough for local RPC discovery availability as long as merge rules are deterministic and protect newer process incarnations.

**Alternatives considered**:

- Implement ViewNode consensus: rejected as out of scope and likely to duplicate Raft authority.
- Keep only one ViewNode: rejected by 009 availability goal.
- Require every node to heartbeat every ViewNode forever before peer sync: rejected as a possible optimization, not a sufficient HA design by itself.

## Decision: Registry Merge Order Is node_id -> incarnation -> sequence -> observed_time

**Rationale**: Current ViewNode and StorageNode registries already use heartbeat sequence and observed time to reject stale updates, but they do not model process incarnation / boot epoch. A restarted process may legitimately reset sequence, so incarnation must dominate sequence. `observed_time` is useful for TTL and diagnostics, but cannot be the authority for overwriting a higher incarnation.

**Alternatives considered**:

- Compare only observed_time: rejected because old process updates may arrive later.
- Compare only sequence: rejected because sequence resets after restart.
- Compare endpoint changes as incarnation: rejected because endpoint may be stable across restart and may change for controlled moves.

## Decision: identity_file Is Local Persistent Identity, Not A Preallocated Lease

**Rationale**: `modules/cluster/node_identity.*` already implements `LoadOrCreateNodeIdentity`, first-start creation, restart reuse, mismatch diagnostics, and platform durability contracts. 009 should extend this model rather than require ViewNode preallocation. Missing identity_file is normal for first start; mismatch/corruption is a startup error.

**Alternatives considered**:

- Treat missing identity_file as fatal: rejected because dynamic nodes must start from empty local directories.
- Make ViewNode a global ID allocator: rejected because ViewNode is not strongly consistent in 009.
- Let Metadata dynamic join decide voter locally: rejected because Raft committed membership is the only authority.

## Decision: StorageNode Dynamic Join Is Discovery And Placement Visibility Only

**Rationale**: StorageNode does not participate in Raft quorum. Its dynamic join should create/reuse local identity, register/heartbeat to ViewNode, and become eligible for future placement. Existing objects are not automatically rebalanced and committed manifests remain unchanged.

**Alternatives considered**:

- Add StorageNode joins to Raft log: rejected because StorageNode membership is not consensus voter membership.
- Rebalance old objects immediately: rejected as non-goal and a separate repair/rebalance feature.
- Require full cluster topology in every StorageNode config: rejected because 009 moves toward seed-based dynamic joining.

## Decision: MetadataNode Dynamic Join Is Consensus Membership Join

**Rationale**: New MetadataNode registration to ViewNode only makes it observable. A new MetadataNode must discover the leader, submit a join request, be committed as learner, catch up via log/snapshot, and only later be promoted by committed membership change. Current Raft runtime membership is static through `NodeConfig::peers`; learner support is not yet complete.

**Alternatives considered**:

- Register MetadataNode to ViewNode as voter: rejected as a direct Raft safety violation.
- Change local identity to voter and start participating: rejected because identity file is not membership authority.
- Allow concurrent arbitrary membership changes immediately: rejected unless joint consensus / batched change safety is implemented.

## Decision: Committed Voter Count Must Always Be Odd

**Rationale**: 009 explicitly requires committed voter membership to remain odd. Learners can be present and continue catch-up without counting toward quorum. Promoting one learner from 3 voters would create 4 voters and is therefore blocked; two ready learners can be promoted together only by a safe membership-change mechanism that never commits 4 voters.

**Alternatives considered**:

- Temporarily commit 4 voters then commit 5 voters: rejected by hard invariant.
- Reject or delete the first learner: rejected because learner should keep catching up and wait for a pair.
- Count learner in quorum while pending: rejected because learner is non-voter by definition.

## Decision: Current Report Is Authoritative For Local RPC Baseline, But Not Complete For Test Indexing

**Rationale**: The requested report confirms the real example, validation scripts, app targets, and self-liveness issue. It does not enumerate every CTest target/label or every test file required by the user prompt. The plan therefore treats report-confirmed facts as baseline evidence and supplements missing test-index facts from `tests/CMakeLists.txt` and targeted source/test inspection.

**Alternatives considered**:

- Guess missing test paths: rejected.
- Stop planning because the report is incomplete: rejected because the local repository contains precise test/build entry points and the user requested a complete spec-kit output.
- Scan the entire repository: rejected by AGENTS.md; inspection was limited to relevant modules, examples, tests, and CMake files.

## Decision: Validation Stays Targeted

**Rationale**: 009 touches high-risk Raft membership and ViewNode/StorageNode discovery. Validation should start from targeted targets and labels before any broad test group. Full test runs are optional closure evidence, not the default first action.

**Alternatives considered**:

- Default full build/test: rejected by user validation guidance.
- Use standalone compiler invocations: rejected because CMake/CTest wiring already exists.
- Print full logs to chat or docs: rejected by project test-log rules.
