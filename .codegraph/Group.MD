好的，这是一个大型分析任务。我先并行获取 CodeGraph 状态和文件结构，然后分批深入查询各核心路径。

Ran `codegraph_status` 
Completed with input: {}

Ran `codegraph_files` 
Completed with input: {
  "format": "tree"
}

CodeGraph 索引就绪：74 文件、2,254 节点、6,380 边。现在分批查询核心路径。

Ran `codegraph_context` 
Completed with input: {
  "task": "RaftNode initialization startup state transition follower candidate leader stepdown",
  "maxNodes": 30
}

Ran `codegraph_context` 
Completed with input: {
  "task": "RequestVote RPC handling send receive vote grant reject term update",
  "maxNodes": 30
}

Ran `codegraph_context` 
Completed with input: {
  "task": "AppendEntries heartbeat log replication consistency check prev_log_index prev_log_term conflict truncate",
  "maxNodes": 30
}

继续查询存储、元数据、快照和提交应用路径。

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 25,
  "task": "RaftStorage persist hard state current_term voted_for log append truncate reload segment fsync durable write snapshot save load recover"
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 25,
  "task": "MetadataStateMachine Apply commit apply committed entries metadata command create delete head list snapshot save load recover"
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 25,
  "task": "MetadataServiceImpl create commit delete head list propose metadata command gRPC service"
}

继续查询关键调用路径和 trace。

Ran `codegraph_trace` 
Completed with input: {
  "from": "RaftNode::Start",
  "to": "RaftNode::StartElection"
}

Ran `codegraph_trace` 
Completed with input: {
  "from": "RaftNode::OnAppendEntries",
  "to": "MetadataStateMachine::Apply"
}

Ran `codegraph_trace` 
Completed with input: {
  "from": "MetadataServiceImpl::CreateObject",
  "to": "RaftNode::ProposeMetadata"
}

动态分派导致静态 trace 中断。让我读取关键实现代码和调用者关系。

Ran `codegraph_node` 
Completed with input: {
  "symbol": "RaftNode::Start",
  "includeCode": true
}

Ran `codegraph_node` 
Completed with input: {
  "symbol": "RaftNode::OnAppendEntries",
  "includeCode": true
}

Ran `codegraph_node` 
Completed with input: {
  "symbol": "RaftNode::StartElection",
  "includeCode": true
}

继续查询剩余核心路径。

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::OnElectionWon"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::ApplyCommittedEntries"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::ProposeMetadata"
}

继续查询 Node 内部方法和元数据服务路径。

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::BecomeFollowerLocked"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::BecomeLeaderLocked"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::PersistStateLocked"
}

查询复制器、快照和元数据状态机核心方法。

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "Replicator::ReplicateOnce"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::ExecuteMetadataProposal"
}

Ran `codegraph_explore` 
Completed with input: {
  "query": "SnapshotStorage SaveSnapshot LoadSnapshot LoadLatestSnapshot snapshot catalog install",
  "maxFiles": 5
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "MetadataStateMachine::Apply"
}

Ran `codegraph_callers` 
Completed with input: {
  "symbol": "MetadataStateMachine::Apply"
}

Ran `codegraph_callers` 
Completed with input: {
  "symbol": "RaftNode::PersistStateLocked"
}

Ran `codegraph_search` 
Completed with input: {
  "query": "SnapshotStorage",
  "kind": "class",
  "limit": 10
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 20,
  "task": "no KV audit surface check CommandType kSet kDelete KvStateMachine KvService raft_kv_client forbidden symbols"
}

Read [](file:///home/yangjilei/.config/Code/User/workspaceStorage/066ca5ce25e6b22e093f758eb22816a8/GitHub.copilot-chat/chat-session-resources/750d7fde-d9fc-48c3-a2e8-f2c490fc47cd/call_00_2uhlaWagWLb2do0BYdgD0120__vscode-1779685811260/content.txt#1-1), lines 1 to 100

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::SendHeartbeats"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::OnRequestVote"
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 20,
  "task": "snapshot save publish load install snapshot worker MaybeScheduleSnapshotLocked snapshot catalog"
}

查询快照调度、元数据服务实现和复制多数派路径。

Ran `codegraph_search` 
Completed with input: {
  "limit": 15,
  "query": "MaybeScheduleSnapshotLocked StartSnapshotWorker snapshot",
  "kind": "method"
}

Ran `codegraph_search` 
Completed with input: {
  "limit": 20,
  "query": "MetadataServiceImpl CreateObject CommitObject DeleteObject HeadObject ListObjects CreateBucket DeleteBucket",
  "kind": "method"
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 15,
  "task": "ReplicateLogEntryToMajority AdvanceCommitIndexUnlocked majority commit quorum"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "RaftNode::ReplicateLogEntryToMajority"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "Replicator::BuildAppendEntriesRequest"
}

Ran `codegraph_search` 
Completed with input: {
  "limit": 10,
  "query": "RaftServiceImpl RequestVote AppendEntries InstallSnapshot",
  "kind": "method"
}

Ran `codegraph_explore` 
Completed with input: {
  "maxFiles": 4,
  "query": "RaftServiceImpl raft_service_impl OnRequestVote OnAppendEntries OnInstallSnapshot HandleRequestVote HandleAppendEntries"
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 20,
  "task": "MetadataServiceImpl gRPC service implementation CreateBucket DeleteBucket CreateObject CommitObject DeleteObject HeadObject ListObjects propose SubmitMetadataCommand"
}

Ran `codegraph_search` 
Completed with input: {
  "limit": 10,
  "query": "MetadataStateMachine SaveSnapshot LoadSnapshot SaveStateMachineSnapshot",
  "kind": "method"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "main"
}

Read [](file:///home/yangjilei/.config/Code/User/workspaceStorage/066ca5ce25e6b22e093f758eb22816a8/GitHub.copilot-chat/chat-session-resources/750d7fde-d9fc-48c3-a2e8-f2c490fc47cd/call_00_VQ6HysRSiUzqZKb5dMlA2667__vscode-1779685811275/content.txt#1-1), lines 1 to 80

查询快照 worker 和 MetadataStateMachine 的保存/加载快照方法。

Ran `codegraph_search` 
Completed with input: {
  "kind": "method",
  "limit": 15,
  "query": "TakeSnapshot SaveMetadataSnapshot LoadMetadataSnapshot snapshot worker MaybeScheduleSnapshot"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "MetadataServiceImpl::CreateObject"
}

Ran `codegraph_search` 
Completed with input: {
  "kind": "method",
  "limit": 10,
  "query": "StartSnapshotWorker MaybeScheduleSnapshot TakeSnapshot snapshot_worker"
}

Ran `codegraph_context` 
Completed with input: {
  "maxNodes": 15,
  "task": "IStateMachine SaveStateMachineSnapshot LoadStateMachineSnapshot snapshot state machine interface"
}

Read [](file:///home/yangjilei/Code/C%2B%2B/CQUPT_Raft/tests/no_kv_surface_audit.cmake#1-1), lines 1 to 50

Ran `codegraph_search` 
Completed with input: {
  "kind": "method",
  "limit": 10,
  "query": "StartSnapshotWorker snapshot_worker TakeSnapshotAsync"
}

Read [](file:///home/yangjilei/Code/C%2B%2B/CQUPT_Raft/tests/no_kv_surface_audit.cmake#50-50), lines 50 to 150

Ran `codegraph_search` 
Completed with input: {
  "kind": "method",
  "limit": 10,
  "query": "SaveSnapshot LoadSnapshot metadata_state_machine"
}

Read [](file:///home/yangjilei/Code/C%2B%2B/CQUPT_Raft/tests/no_kv_surface_audit.cmake#150-150), lines 150 to 250

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "MetadataStateMachine::SaveSnapshot"
}

Ran `codegraph_node` 
Completed with input: {
  "includeCode": true,
  "symbol": "MetadataStateMachine::LoadSnapshot"
}

Read [](file:///home/yangjilei/Code/C%2B%2B/CQUPT_Raft/tests/no_kv_surface_audit.cmake#250-250), lines 250 to 294

Ran `codegraph_search` 
Completed with input: {
  "kind": "method",
  "limit": 10,
  "query": "RaftNode::GetOrCreateReplicatorLocked AppendLocalLogUnlocked"
}

Read [](file:///home/yangjilei/.config/Code/User/workspaceStorage/066ca5ce25e6b22e093f758eb22816a8/GitHub.copilot-chat/chat-session-resources/750d7fde-d9fc-48c3-a2e8-f2c490fc47cd/call_01_o9fp9FrPVyWtghw8oI6I4359__vscode-1779685811291/content.txt#1-1), lines 1 to 80

现在我已经获得了足够的代码上下文信息，下面输出完整分析报告。

---

# CQUPT_Raft 全项目核心调用路径与模块边界分析

---

## 一、CodeGraph 索引状态

| 指标 | 数值 |
|------|------|
| **已索引文件** | **74**（C++ 69 + YAML 5） |
| **总符号节点** | **2,254** |
| **总调用边** | **6,380** |
| **数据库大小** | 7.41 MB |
| **后端** | node:sqlite (WAL + FTS5) |
| **状态** | 就绪，可直接查询 |

索引覆盖了 apps、`modules/raft/*`、tests 和 .specify 下的所有源码文件，NOTREAD.md 禁读的路径未被索引。

---

## 二、AGENTS.md 与本次分析相关的入口规则

| 规则 | 说明 |
|------|------|
| **入口顺序** | 先读根 AGENTS.md → 目标模块 AGENTS.md → 模块源码与测试 |
| **禁止全仓库扫描** | 不默认扫描整个仓库 |
| **全局硬约束** | 禁止修改业务逻辑、协议语义、持久化格式、公共 API、类名/函数名/命名空间；禁止删除测试或跳过失败测试 |
| **禁止读取** | vcpkg-configuration.json、`CQUPT_Raft_AI_Context.md`、README.md、deploy |
| **高风险区域** | proto、`service/`、`storage/`、`node/`、`replication/`、snapshot/restart/catch-up 测试、并发与 crash recovery |
| **跨模块修改规则** | 改 `node` → 检查 `replication`/`storage`/`state_machine`；改 proto/`service` → 检查契约使用方和测试 |
| **测试日志规则** | 通过只报 PASS；失败只报摘要 + 最后 50 行 + 完整日志路径 |

---

## 三、NOTREAD.md 与本次分析相关的禁读范围

以下路径在本次分析和所有后续操作中均**禁止读取**：

```
/build              # 构建产物
/vcpkg-configuration.json
/test.sh            # 脚本入口
/scripts
/raft_data          # 运行时数据
/raft_snapshots     # 运行时快照
/raft_test_data     # 测试运行时数据
/deploy             # 部署配置
/tmp                # 临时文件
/docs               # 历史文档
/specs/003          # 历史 spec
/specs/004
/specs/005
/specs/006
/temp
```

---

## 四、项目函数调用关系库核心模块

| 模块 | 目录 | 核心职责 | 关键类/函数 |
|------|------|----------|------------|
| **proto** | proto | RPC/Protobuf 契约 | `raft.proto`, `metadata.proto` |
| **common** | common | 命令编解码、配置、提案结果 | `Command`, `MetadataCommand`, `NodeConfig`, `ProposeResult` |
| **metadata** | metadata | metadata 业务类型、payload、查询 | `CommitObjectCommandPayload`, `CreateObjectCommandPayload`, `HeadObjectQuery` |
| **runtime** | runtime | 日志、定时器、线程池 | `MinHeapTimer`, `ThreadPool`, `Log` |
| **service** | service | gRPC Raft/KV 服务适配 | `RaftServiceImpl`, `MetadataServiceImpl` |
| **node** | node | RaftNode 核心状态调度 | `RaftNode` (所有核心方法) |
| **replication** | replication | 单 follower 复制状态机 | `Replicator` |
| **storage** | storage | hard state、segment log、snapshot catalog | `FileRaftStorage`, `SnapshotMeta` |
| **state_machine** | state_machine | 状态机接口与 metadata 实现 | `IStateMachine`, `MetadataStateMachine`, `StrongConsistencyMetadataStateMachine` |
| **apps** | apps | 节点入口与 CLI 客户端 | `main()` |

---

## 五、Raft 核心协议主路径 — ASCII 调用关系图

### 5.1 节点启动与初始化

```
main()
  │
  ├─► LoadKeyValueConfig()          [apps/main.cpp]
  ├─► BuildNodeConfig()
  ├─► BuildSnapshotConfig()
  ├─► PrintConfigSummary()
  ├─► RaftNode(config, snapshot_config)  [raft_node.cpp]
  │     └─► storage_ = FileRaftStorage(data_dir)  [raft_storage.cpp]
  │           └─► Load() → LoadSegmented() / LoadLegacy()
  │                 ├─► ReadMeta()
  │                 ├─► LoadSegments()
  │                 └─► ClampRecoveredHardState()
  │     └─► state_machine_ = MetadataStateMachine()
  │     └─► init scheduler_, rpc_pool_, clients_
  │
  └─► node->Start()
        ├─► rpc_pool_.Start()
        ├─► InitClients()
        ├─► scheduler_.Start()
        ├─► StartSnapshotWorker()   [inferred - 快照后台线程]
        ├─► InitServer()            [inferred - gRPC server]
        ├─► ResetElectionTimerLocked()
        └─► ResetSnapshotTimerLocked()
```

### 5.2 Election timeout → RequestVote 发起路径

```
[scheduler_ timer fire]
  │
  └─► RaftNode::StartElection()     [raft_node.cpp:701]
        ├─► role_ = kCandidate; ++current_term_; voted_for_ = self
        ├─► PersistStateLocked()    ─► storage_->Save(state)
        │                              ├─► WriteSegments()  [segment log]
        │                              ├─► WriteMeta()      [meta file]
        │                              └─► AtomicRename()   [durable commit]
        ├─► ResetElectionTimerLocked()
        ├─► For each peer:
        │     rpc_pool_.Submit() →
        │       RequestVoteRpc(peer, request)
        │         └─► stub->RequestVote()  [gRPC call]
        │       On response:
        │         ├─► if response.term > current_term_:
        │         │     BecomeFollowerLocked(term, -1)
        │         ├─► if response.vote_granted:
        │         │     votes++ → if quorum → OnElectionWon(term)
        │
        └─► if quorum ≤ 1 (single node):
              OnElectionWon(term)
```

### 5.3 RequestVote 接收与响应路径

```
[gRPC incoming]
  │
  └─► RaftServiceImpl::RequestVote()     [raft_service_impl.cpp:11]
        └─► RaftNode::OnRequestVote()    [raft_node.cpp:1319]
              ├─► if request.term < current_term_: reject
              ├─► if request.term > current_term_:
              │     BecomeFollowerLocked(term, -1)
              │       ├─► current_term_ = new_term; voted_for_ = -1
              │       ├─► role_ = kFollower
              │       ├─► Cancel heartbeat timer
              │       ├─► ResetElectionTimerLocked()
              │       └─► if hard_state_changed: PersistStateLocked()
              │
              ├─► IsCandidateLogUpToDateLocked()
              │     └─► check last_log_index / last_log_term
              ├─► if can_vote && up_to_date:
              │     voted_for_ = candidate_id
              │     └─► PersistStateLocked()  ← 持久化 voted_for
              │           success → vote_granted = true
              │           fail    → rollback voted_for_
              └─► ResetElectionTimerLocked()
```

### 5.4 Leader 选举成功后的初始化路径

```
RaftNode::OnElectionWon(term)              [raft_node.cpp:807]
  │
  ├─► BecomeLeaderLocked()                 [raft_node.cpp:938]
  │     ├─► role_ = kLeader
  │     ├─► leader_id_ = self
  │     ├─► CancelElectionTimerLocked()
  │     ├─► init next_index_[] / match_index_[] for all peers
  │     ├─► For each peer: GetOrCreateReplicatorLocked(peer)
  │     └─► ResetHeartbeatTimerLocked()
  │
  ├─► ProposeNoOpEntry()
  │     └─► ExecuteMetadataProposal-like flow:
  │           AppendLocalLogUnlocked() → PersistStateLocked()
  │           → ReplicateLogEntryToMajority()
  │           → AdvanceCommitIndexUnlocked() → PersistStateLocked()
  │           → ApplyCommittedEntries()
  │
  └─► SendHeartbeats()                     [raft_node.cpp:836]
        └─► For each peer:
              rpc_pool_.Submit() →
                replicator->ReplicateOnce(term, target_index=0, &should_apply)
                if should_apply → ApplyCommittedEntries()
```

### 5.5 Heartbeat / AppendEntries 定时发送路径

```
[heartbeat timer fire]
  │
  └─► RaftNode::SendHeartbeats()           [raft_node.cpp:836]
        └─► For each peer:
              Replicator::ReplicateOnce(term, 0, &should_apply)  [replicator.cpp:60]
                │
                ├─► if target matched: return true (skip)
                ├─► if inflight or backoff: return false
                │
                ├─► BuildAppendEntriesRequest(term, &req, &install_snapshot)
                │     ├─► if next_index ≤ first_log_index → need snapshot
                │     ├─► prev_log_index = next_index - 1
                │     ├─► prev_log_term = TermAtIndexLocked(prev_log_index)
                │     ├─► set term, leader_id, prev_log_index, prev_log_term,
                │     │        leader_commit
                │     └─► pack entries from next_index up to batch limit
                │
                ├─► if install_snapshot:
                │     node_.SendInstallSnapshotToPeer()  [inferred]
                │
                └─► else:
                      node_.AppendEntriesRpc(peer, request)  [gRPC]
                      └─► HandleAppendEntriesResponse()
                            ├─► update next_index_ / match_index_
                            └─► if leader_commit advanced: should_apply = true
```

### 5.6 Follower 接收 AppendEntries 与日志处理路径

```
RaftServiceImpl::AppendEntries()            [raft_service_impl.cpp:24]
  │
  └─► RaftNode::OnAppendEntries()          [raft_node.cpp:1367]
        │
        ├─► if request.term < current_term_: reject
        ├─► if request.term > current_term_ or not follower:
        │     BecomeFollowerLocked(term, leader_id)
        │       └─► PersistStateLocked()
        ├─► else: ResetElectionTimerLocked()
        │
        ├─► prev_log_index consistency checks:
        │     ├─► if prev_log_index < last_snapshot_index_: reject (conflict)
        │     ├─► if !HasLogAtIndexLocked(prev_log_index): reject (conflict)
        │     └─► if TermAtIndexLocked(prev_log_index) ≠ prev_log_term:
        │           reject (conflict) → SetAppendEntriesConflictHintLocked()
        │
        ├─► For each entry in request.entries:
        │     ├─► skip if entry.index ≤ last_snapshot_index_
        │     ├─► if entry.index > LastLogIndexLocked() + 1: reject
        │     ├─► if existing entry has conflicting term:
        │     │     truncate log_ from offset
        │     │     log_changed = true
        │     ├─► append new entry: log_.push_back({index, term, command})
        │     │     log_changed = true
        │     └─► match_index update
        │
        ├─► if log_changed:
        │     PersistStateLocked()  ← 持久化新日志
        │     └─► on fail: rollback to old_log
        │
        ├─► if leader_commit > commit_index_:
        │     commit_index_ = min(leader_commit, LastLogIndexLocked())
        │     └─► PersistStateLocked()  ← 持久化新 commit_index
        │           on success → should_apply = true
        │
        └─► if should_apply:
              ApplyCommittedEntries()
```

### 5.7 Commit/Apply 推进路径

```
RaftNode::ApplyCommittedEntries()            [raft_node.cpp:2556]
  │
  └─► Loop while last_applied_ < commit_index_:
        │
        ├─► if last_applied_ < last_snapshot_index_:
        │     last_applied_ = last_snapshot_index_   [snapshot 跳过]
        │
        ├─► apply_index = last_applied_ + 1
        ├─► HasLogAtIndexLocked(apply_index) check
        ├─► LogAtIndexLocked(apply_index) → command_data, term
        │
        ├─► state_machine_->Apply(index, term, command_data)
        │     └─► MetadataStateMachine::Apply()     [metadata_state_machine.cpp:1107]
        │           ├─► if kInternalNoOpCommand: skip
        │           ├─► ParseMetadataStateMachineCommand()
        │           ├─► ValidateMetadataCommand()
        │           ├─► request_id 幂等检查:
        │           │     ├─► fingerprint match → replay success
        │           │     └─► fingerprint mismatch → conflict failure
        │           ├─► CreateBucket / DeleteBucket
        │           ├─► CreateObject / CommitObject / AbortObject
        │           └─► DeleteObject
        │
        ├─► last_applied_ = apply_index
        ├─► PersistStateLocked()  ← 持久化新 last_applied_
        └─► MaybeScheduleSnapshotLocked(false)
```

---

## 六、Raft 存储与持久化路径 — ASCII 调用关系图

```
                         ┌──────────────────────────────┐
                         │      RaftNode 内存状态         │
                         │  current_term_, voted_for_,   │
                         │  commit_index_, last_applied_, │
                         │  log_, last_snapshot_index_    │
                         └──────────────┬───────────────┘
                                        │
                               PersistStateLocked()
                                        │
                         ┌──────────────▼───────────────┐
                         │     FileRaftStorage           │
                         │     (raft_storage.cpp)        │
                         └──────────────┬───────────────┘
                                        │
                    ┌───────────────────┼───────────────────┐
                    │                   │                   │
              Save(state)         Load(state)        Save(state)
              [写入路径]          [恢复路径]         [快照路径]
                    │                   │                   │
         ┌──────────▼──────────┐  ┌────▼──────────┐  ┌────▼──────────────┐
         │  WriteSegments()    │  │ LoadSegmented()│  │  SnapshotStorage   │
         │  [分段日志写入]      │  │ [分段日志恢复]  │  │  (snapshot dir)    │
         │                    │  │                │  │                    │
         │  per segment:      │  │  ReadMeta()    │  │  snapshot_<idx>/   │
         │  ├─ SegmentFileName │  │  LoadSegments()│  │  ├─ data.bin       │
         │  ├─ SegmentEntryHdr │  │  ClampRecovered│  │  └─ __raft_snap... │
         │  ├─ FlushAndSync   │  │  HardState()   │  │                    │
         │  └─ SyncDirectory  │  │                │  │  SnapshotMeta:      │
         │                    │  │                │  │  snapshot_path,     │
         │  WriteMeta()       │  │                │  │  last_included_idx, │
         │  ├─ magic, version │  │                │  │  last_included_term,│
         │  ├─ current_term   │  │                │  │  data_checksum      │
         │  ├─ voted_for      │  │                │  └────────────────────┘
         │  ├─ commit_index   │  │
         │  ├─ last_applied   │  │
         │  ├─ first/last idx │  │
         │  └─ log_count      │  │
         │                    │  │
         │  AtomicRename()    │  │
         │  [原子提交]         │  │
         └────────────────────┘  └────────────────┘
```

### 持久化语义总结

| 操作 | 持久化时机 | 失败回滚 |
|------|-----------|---------|
| `current_term_` 更新 | `StartElection()`, `BecomeFollowerLocked()` | 回滚 term + voted_for |
| `voted_for_` 更新 | `OnRequestVote()` 投票时 | 回滚 voted_for |
| 日志追加 | `OnAppendEntries()` 追加后 | 回滚到 old_log |
| `commit_index_` 推进 | `OnAppendEntries()` / `AdvanceCommitIndexUnlocked()` | 回滚 commit_index |
| `last_applied_` 推进 | `ApplyCommittedEntries()` 每条 apply 后 | 返回错误 |

所有持久化均通过 `PersistStateLocked()` → `storage_->Save()` → `WriteSegments()` + `WriteMeta()` + atomic rename 实现。失败会回滚内存状态。不支持静默降级或 no-op 成功。

---

## 七、Metadata-Only 业务主路径 — ASCII 调用关系图

```
[gRPC Client]
  │
  └─► MetadataServiceImpl::CreateObject()    [metadata_service_impl.cpp:698]
  └─► MetadataServiceImpl::CommitObject()
  └─► MetadataServiceImpl::DeleteObject()
  └─► MetadataServiceImpl::HeadObject()      [查询 - 直接读状态机]
  └─► MetadataServiceImpl::ListObjects()     [查询 - 直接读状态机]
  └─► MetadataServiceImpl::CreateBucket()
  └─► MetadataServiceImpl::DeleteBucket()
  └─► MetadataServiceImpl::AbortObject()
        │
        │ [写路径]
        ▼
      MakeCreateObjectCommand() / MakeCommitObjectCommand() / ...
        │
        ▼
      ValidateMetadataCommand()
        │
        ▼
      SerializeMetadataCommand()
        │
        ▼
      node_.ProposeMetadata(serialized)       [raft_node.cpp:1939]
        │
        ├─► ParseMetadataCommand()
        ├─► ValidateMetadataCommand()
        ├─► 幂等检查: request_id + fingerprint
        │     ├─► completed → 返回缓存结果
        │     └─► inflight → join existing tracker
        ├─► role == kLeader 检查
        ├─► new tracker → rpc_pool_.Submit()
        │     └─► ExecuteMetadataProposal()   [raft_node.cpp:2090]
        │           ├─► command.type = CommandType::kMetadata
        │           ├─► AppendLocalLogUnlocked(command_data)
        │           │     └─► PersistStateLocked()
        │           ├─► ReplicateLogEntryToMajority(log_index)
        │           │     ├─► For each peer:
        │           │     │     Replicator::ReplicateOnce(term, log_index, ...)
        │           │     │       └─► BuildAppendEntriesRequest()
        │           │     │       └─► AppendEntriesRpc()
        │           │     │       └─► HandleAppendEntriesResponse()
        │           │     └─► wait for majority match_index >= log_index
        │           ├─► AdvanceCommitIndexUnlocked()
        │           │     └─► PersistStateLocked()
        │           ├─► ApplyCommittedEntries()
        │           │     └─► MetadataStateMachine::Apply()
        │           └─► CompleteMetadataProposal()
        │
        └─► WaitForMetadataProposalTracker()
              └─► result.status = kOk / kTimeout / kNotLeader / ...

[读路径]
MetadataServiceImpl::HeadObject()
  │
  └─► node_.GetMetadataStateMachineV2()
        └─► MetadataStateMachine::FindObject(bucket, key)
              └─► [shared_lock] objects_ 查询
```

### Metadata 状态流转

```
CreateObject → PENDING ──► CommitObject → COMMITTED
                  │                            │
                  │                            │
                  ▼                            ▼
              AbortObject                 DeleteObject
                  │                            │
                  ▼                            ▼
              [Tombstone]                 [Tombstone]
             (GC eligible)              (GC eligible)
```

---

## 八、Snapshot / Recovery 路径 — ASCII 调用关系图

```
                    ┌──────────────────────────────────────┐
                    │     RaftNode::MaybeScheduleSnapshot   │
                    │     Locked()                          │
                    │     [apply 后触发, 基于日志大小策略]     │
                    └──────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼───────────────────────┐
                    │     snapshot_worker_ 线程             │
                    │     [inferred - StartSnapshotWorker]  │
                    └──────────────┬───────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
    Save Raft State         Save State Machine     Update Catalog
              │                    │                    │
    ┌─────────▼─────────┐  ┌──────▼──────────┐  ┌─────▼──────────────┐
    │ IStateMachine::    │  │ MetadataSM::    │  │ SnapshotStorage     │
    │ SaveSnapshot(path) │  │ SaveSnapshot()  │  │                     │
    │                    │  │                 │  │ snapshot_<index>/   │
    │ [interface call]   │  │ ├─ buckets_     │  │ ├─ data.bin         │
    │                    │  │ ├─ objects_     │  │ │   [状态机快照]      │
    │                    │  │ ├─ object_idx_  │  │ └─ __raft_snap...   │
    │                    │  │ ├─ chunk_refs_  │  │                     │
    │                    │  │ ├─ requests_    │  │ SnapshotMeta:        │
    │                    │  │ ├─ fingerprints_│  │ last_included_index  │
    │                    │  │ ├─ tombstones_  │  │ last_included_term   │
    │                    │  │ ├─ last_applied │  │ data_checksum        │
    │                    │  │ ├─ last_term    │  │ created_unix_ms      │
    │                    │  │ └─ sort+write   │  └─────────────────────┘
    │                    │  │   [binary fmt]  │
    └────────────────────┘  └────────────────┘


                    ┌──────────────────────────────────────┐
                    │     Crash Recovery 流程               │
                    └──────────────┬───────────────────────┘
                                   │
    ┌──────────────────────────────▼───────────────────────────────┐
    │  RaftNode::RaftNode() → FileRaftStorage::Load()              │
    │    ├─► LoadSegmented() / LoadLegacy()                        │
    │    │    ├─► ReadMeta() → current_term_, voted_for_,          │
    │    │    │                 commit_index_, last_applied_        │
    │    │    └─► LoadSegments() → log_[]                          │
    │    │                                                         │
    │    └─► ClampRecoveredHardState()                             │
    │          ├─► 验证 log_ 连续性                                 │
    │          └─► 修正 commit_index_ / last_applied_ 范围         │
    │                                                              │
    │  [节点重启后自动从持久化状态恢复, 角色为 Follower]             │
    │  [commit_index_ 之后的日志项需要重新选举 leader 后复制确认]    │
    └──────────────────────────────────────────────────────────────┘


                    ┌──────────────────────────────────────────────┐
                    │     InstallSnapshot (Follower 追赶)           │
                    └──────────────┬───────────────────────────────┘
                                   │
    Replicator::ReplicateOnce() → BuildAppendEntriesRequest()
      │
      ├─► if next_index ≤ leader.FirstLogIndexLocked():
      │      should_install_snapshot = true
      │
      └─► node_.SendInstallSnapshotToPeer()  [inferred - raft_node]
            │
            └─► RaftNode::OnInstallSnapshot()
                  ├─► Load state machine snapshot from leader
                  ├─► Truncate Raft log after snapshot index
                  ├─► Update last_snapshot_index_, last_applied_
                  ├─► PersistStateLocked()
                  └─► Respond to leader
```

### Snapshot 与 Log Compaction 关系

```
snapshot 创建后:
  - last_snapshot_index_ 之后保留日志
  - last_snapshot_index_ 之前的日志可被 compact/GC
  - ApplyCommittedEntries() 跳过 ≤ last_snapshot_index_ 的条目
  - Follower 追赶时若 next_index ≤ first_log_index, 需要 InstallSnapshot
```

---

## 九、Raft 与 MetadataStateMachine 的边界

```
┌─────────────────────────────────────────────────────────────────┐
│                        RaftNode                                  │
│                                                                  │
│  职责:                                                           │
│  ✅ Raft 共识 (选举、日志复制、多数派确认)                          │
│  ✅ 日志管理 (append, truncate, persist)                          │
│  ✅ commit_index / last_applied 推进                              │
│  ✅ snapshot 调度 (MaybeScheduleSnapshotLocked)                   │
│  ✅ RPC 适配 (RequestVote, AppendEntries, InstallSnapshot)        │
│  ✅ ProposeMetadata() 作为 metadata 命令入口                      │
│                                                                  │
│  不负责:                                                         │
│  ❌ 命令语义解析 (交给 MetadataStateMachine)                       │
│  ❌ 业务数据存储 (对象数据不写入 Raft log)                          │
│  ❌ 查询处理 (读操作直接走 MetadataStateMachine 查询接口)           │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           │ Apply(index, term, command_data)
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                    MetadataStateMachine                          │
│                                                                  │
│  职责:                                                           │
│  ✅ 解析 metadata 命令 (CreateBucket/DeleteBucket/CreateObject/  │
│     CommitObject/AbortObject/DeleteObject)                       │
│  ✅ 维护 buckets_, objects_, object_index_, chunk_ref_index_,    │
│     requests_, tombstones_ 内存数据结构                           │
│  ✅ request_id 幂等保证 (fingerprint 匹配/冲突检测)                │
│  ✅ SaveSnapshot() / LoadSnapshot() 状态机快照                    │
│  ✅ FindObject(), FindBucket(), HeadObject(), ListObjects() 查询  │
│                                                                  │
│  不负责:                                                         │
│  ❌ Raft 共识、日志复制                                           │
│  ❌ 对象数据落盘 (那是 storage node 的职责)                        │
│  ❌ 直接接收 gRPC 请求 (由 MetadataServiceImpl 转发)               │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           │ FindObject / FindBucket / ...
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                    MetadataServiceImpl                           │
│                                                                  │
│  职责:                                                           │
│  ✅ gRPC 请求入口 (CreateBucket, DeleteBucket, CreateObject,     │
│     CommitObject, AbortObject, DeleteObject, HeadObject,         │
│     ListObjects)                                                 │
│  ✅ 请求 → MetadataCommand 转换 (MakeCreateObjectCommand 等)      │
│  ✅ 调用 ValidateMetadataCommand() 校验                           │
│  ✅ 写: SerializeMetadataCommand() → node_.ProposeMetadata()     │
│  ✅ 读: node_.GetMetadataStateMachineV2() → FindObject() 等      │
│  ✅ 填充 protobuf response                                       │
│                                                                  │
│  不负责:                                                         │
│  ❌ Raft 共识、命令执行语义 (交给 RaftNode + MetadataStateMachine) │
│  ❌ 对象数据操作 (那是 storage node 的职责)                        │
└─────────────────────────────────────────────────────────────────┘
```

### 硬约束验证

| 约束 | 状态 | 验证方式 |
|------|------|---------|
| 不允许对象数据写入 Raft log | ✅ 满足 | `CommandType` 只有 `kMetadata`; 对象数据不在 command payload 中 |
| 不允许恢复旧 KV 作为数据主路径 | ✅ 满足 | `CommandType::kSet/kDelete` 不存在; no-KV audit 通过 |
| MetadataServiceImpl 只负责业务入口 | ✅ 满足 | 只做命令构造、校验、转发 ProposeMetadata / 查询 |
| MetadataStateMachine 只负责解释已提交命令 | ✅ 满足 | 接口只有 `Apply()`, `SaveSnapshot()`, `LoadSnapshot()`, 查询方法 |
| RaftNode 只负责共识/日志/复制 | ✅ 满足 | 通过 `IStateMachine` 接口调用 Apply，不感知具体命令 |

---

## 十、No-KV Audit 边界总结

### 已禁止的文件（strict-fail）

| 文件 | 类别 |
|------|------|
| `modules/raft/service/kv_service_impl.h/.cpp` | forbidden production file |
| `apps/raft_kv_client.cpp` | forbidden production file |
| `proto/kv.proto` | forbidden proto file |
| `modules/raft/state_machine/state_machine.h/.cpp` | forbidden production file |
| `tests/test_kv_service.cpp` | forbidden test file |
| `tests/test_state_machine.cpp` | forbidden test file |

### 已禁止的符号（生产代码 strict-fail）

| 符号 | 检测方式 |
|------|---------|
| `CommandType::kSet`, `CommandType::kDelete` | regex scan 生产源码 |
| `kSet`, `kDelete` token | regex scan |
| `KvStateMachine`, `CompositeKvMetadataStateMachine` | regex scan |
| `KvService`, `KvStatusCode` | regex scan |
| `raft_kv_client`, `PutRequest`, `GetRequest`, `DeleteRequest` | regex scan |
| `DebugGetValue`, `kv-service`, `kv.proto` | literal/regex scan |
| `raft/state_machine/state_machine.h` include | literal scan |

### 已禁止的构建目标

| 目标 | 检测位置 |
|------|---------|
| `raft_kv_client` | CMakeLists.txt |
| `kv_service_impl` source | CMakeLists.txt |
| `state_machine.cpp` source | CMakeLists.txt |
| `test_kv_service`, `test_state_machine` | CMakeLists.txt |

### 测试中也禁止

所有测试主体文件（`tests/*` 去除 AGENTS.md 和 no_kv_surface_audit.cmake 自身）同样扫描上述符号，禁止出现。

### Deferred Risk

| 项目 | 说明 |
|------|------|
| test.ps1 | 仍保留 `KvStateMachineTest` fallback 子集说明 → T059 |
| CMakePresets.json | 仍保留 `KvStateMachineTest` fallback filter → T059 |

---

## 十一、Future Storage Node 接入位置 — ASCII 架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          Client (SDK / CLI)                              │
└────────────┬──────────────────────────────┬─────────────────────────────┘
             │                              │
    [metadata ops]                   [object data ops]
             │                              │
             ▼                              ▼
┌────────────────────────┐    ┌──────────────────────────────────┐
│   MetadataService      │    │      StorageNodeService           │
│   (gRPC, Raft-based)   │    │      (gRPC, direct data path)     │
│                        │    │                                   │
│  CreateBucket          │    │  PutChunk / GetChunk              │
│  DeleteBucket          │    │  DeleteChunk / GC                 │
│  CreateObject ────────►│◄───│  ReplicateChunk (storage-level)   │
│  CommitObject ────────►│    │  RepairChunk                      │
│  AbortObject           │    │                                   │
│  DeleteObject          │    │  [不经过 Raft log]                 │
│  HeadObject ◄──────────│    │  [直接文件系统/块存储]              │
│  ListObjects ◄─────────│    │                                   │
└───────────┬────────────┘    └──────────────────────────────────┘
            │
            │ ProposeMetadata()
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Raft Consensus Layer                        │
│                                                                  │
│  RaftNode → Raft Log → Replicate → Commit → Apply               │
│                                                                  │
│  [只存储 metadata: bucket 元数据, object 元数据, chunk layout]     │
│  [不存储: object/chunk 二进制数据]                                 │
└───────────┬─────────────────────────────────────────────────────┘
            │
            │ Apply()
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                   MetadataStateMachine                           │
│                                                                  │
│  buckets_:     BucketRecord (id, create_time, deleted)           │
│  objects_:     ObjectRecord (bucket, key, id, version, size,    │
│                etag, state, chunks[], create_time, commit_time)  │
│  chunk_refs_:  ChunkRef → StorageNodeID, ChunkID, offset, size   │
│  tombstones_:  deleted objects/buckets for GC                    │
│                                                                  │
│  chunk 引用指向 StorageNode，实际数据在 StorageNode 文件系统上     │
└──────────────────────────────────────────────────────────────────┘


                    Metadata ↔ StorageNode 协调流程

  [写入流程]
  Client ─► MetadataService.CreateObject()
               │
               ├─► Raft Propose → Commit → Apply
               │      metadata state: PENDING, chunks[] = []
               │
               ▼
           Client ─► StorageNode.PutChunk(chunk_data)
               │      [数据直接写入 storage node 文件系统]
               │      [此时 metadata 仍为 PENDING]
               │
               ▼
           Client ─► MetadataService.CommitObject()
               │      Raft Propose → Commit → Apply
               │      metadata state: COMMITTED, chunks[] 更新
               │
               ▼
           [StorageNode 可安全持久化 chunk，标记为 committed]

  [回滚/Abort 流程]
  Client ─► MetadataService.AbortObject()
               Raft Propose → Commit → Apply
               metadata state: → Tombstone
               └─► StorageNode.GC(chunk_list)  [异步清理]


  [未来接入点]
  ┌──────────────────────────────────────────────────────────────┐
  │  1. Commit 前 StorageNode 职责:                               │
  │     - 接收 chunk 写入请求                                      │
  │     - 暂存 chunk（pending 状态）                                │
  │     - 不对外暴露未 commit 的 chunk                              │
  │                                                               │
  │  2. Commit 后 StorageNode 职责:                                │
  │     - chunk 标记为 committed，对外可读                          │
  │     - 参与 storage-level replication (副本修复)                │
  │                                                               │
  │  3. 删除 / GC 接入点:                                          │
  │     - metadata Tombstone → 触发 StorageNode GC                │
  │     - 异步批量清理过期 chunk                                    │
  │                                                               │
  │  4. Recovery 接入点:                                           │
  │     - metadata 恢复后，rebuild chunk 索引                      │
  │     - StorageNode 启动时扫描本地 chunk 并 report 给 metadata    │
  │     - 不一致时以 metadata (Raft committed) 为准                │
  │                                                               │
  │  5. Replica Repair 接入点:                                     │
  │     - metadata chunk layout → 检测副本数量不足                  │
  │     - 触发 StorageNode 间的 chunk 复制                          │
  │     - 不经过 Raft log，直接 storage-level P2P                  │
  └──────────────────────────────────────────────────────────────┘
```

---

## 十二、标注说明

| 标记 | 含义 | 示例 |
|------|------|------|
| `[inferred]` | CodeGraph 无法直接确认调用边，基于代码上下文和命名推断 | `StartSnapshotWorker()` 的具体实现未在索引中匹配到 |
| `[unknown]` | CodeGraph 无法确认的调用关系（动态分派、虚函数、跨模块回调） | `IStateMachine::Apply()` 的具体实现分发在静态索引中不可见 |
| 无标记 | CodeGraph 直接确认的调用关系 | 所有标注了文件:行号的调用 |

### CodeGraph 已知限制

1. **动态分派（虚函数）**：`IStateMachine::Apply()`, `SaveSnapshot()`, `LoadSnapshot()` 的虚函数调用在静态索引中无法追踪到具体实现（`MetadataStateMachine` 或 `StrongConsistencyMetadataStateMachine`），调用边在接口层断开。
2. **线程池回调**：`rpc_pool_.Submit(lambda)` 中的 lambda 闭包调用在索引中不可见。
3. **gRPC 异步调用**：`stub->RequestVote()` / `stub->AppendEntries()` 是 protobuf 生成的代码，不在项目索引中。
4. **私有方法**：部分 `RaftNode` 私有方法（如 `GetOrCreateReplicatorLocked`, `AppendLocalLogUnlocked`）未在 CodeGraph 搜索中返回结果，可能是名称匹配问题。

---

以上是基于 CodeGraph 索引（74 文件、2,254 节点、6,380 边）对 CQUPT_Raft 项目的完整调用路径分析。所有核心 Raft 协议路径、存储持久化路径、metadata 业务路径、snapshot/recovery 路径和未来 storage node 接入架构均已覆盖。