# CQUPT_Raft

一个基于 **C++20 + gRPC + Protobuf + GoogleTest** 实现的 Raft KV 存储内核。当前版本重点完成了 Raft 内核能力：选举、日志复制、提交应用、持久化、快照、落后节点追赶、InstallSnapshot、segment log、目录式 snapshot、独立 Replicator 复制状态机，以及较完整的 GTest 覆盖。

当前项目定位是：**先完成单 RaftGroup 的内核稳定性，再往 KV Service / File Storage / Meta 管理方向扩展**。

---

## 1. 当前能力概览

当前已经实现：

- Raft leader election。
- RequestVote / AppendEntries / InstallSnapshot RPC。
- 多数派提交。
- 客户端命令通过 `Propose()` 进入 Raft 日志。
- follower 检查 `prev_log_index / prev_log_term`。
- leader 根据 `last_log_index` 快速回退 `next_index`。
- AppendEntries 批量追赶。
- follower 严重落后时通过 InstallSnapshot 追赶。
- snapshot 安装完成后继续复制 snapshot 之后的日志。
- `Replicator` 独立维护每个 follower 的复制状态。
- 单 follower in-flight 限制。
- RPC 失败指数退避。
- segment 日志持久化。
- snapshot 后自动清理旧 segment。
- 目录式 snapshot 保存。
- snapshot checksum 校验。
- 损坏 snapshot 回退加载旧 snapshot。
- snapshot 后 tail logs 重启恢复。
- 纯 log 重启恢复时 replay 已提交日志。
- leader election timer generation 校验，避免旧 timer 回调导致重复选举。
- GoogleTest 覆盖基础 KV、选举、复制、提交、持久化、快照、追赶、Replicator、segment storage、snapshot storage。

暂时还没有实现：

- 对外 KV gRPC Service。
- 线性一致读 ReadIndex。
- 文件对象存储 StorageNode。
- Meta / Manager 多集群管理。
- 动态成员变更。
- learner。
- snapshot chunk 流式传输。
- 工业级异步 WAL。
- client request 去重。

---

## 2. 项目目录结构

典型目录结构如下：

```text
CQUPT_Raft/
  CMakeLists.txt
  config.txt
  proto/
    raft.proto

  include/raft/
    command.h
    config.h
    logging.h
    min_heap_timer.h
    propose.h
    raft_node.h
    raft_service_impl.h
    raft_storage.h
    replicator.h
    snapshot_storage.h
    state_machine.h
    thread_pool.h

  src/
    command.cpp
    main.cpp
    min_heap_timer.cpp
    raft_node.cpp
    raft_service_impl.cpp
    raft_storage.cpp
    replicator.cpp
    snapshot_storage.cpp
    state_machine.cpp
    thread_pool.cpp

  tests/
    CMakeLists.txt
    test_command.cpp
    test_state_machine.cpp
    test_min_heap_timer.cpp
    test_thread_pool.cpp
    test_raft_election.cpp
    test_raft_log_replication.cpp
    test_raft_commit_apply.cpp
    persistence_test.cpp
    snapshot_test.cpp
    raft_integration_test.cpp
    test_raft_snapshot_catchup.cpp
    test_raft_snapshot_restart.cpp
    test_raft_snapshot_diagnosis.cpp
    test_raft_segment_storage.cpp
    test_snapshot_storage_reliability.cpp
    test_raft_replicator_behavior.cpp
```

运行后会产生：

```text
raft_data/
  node_1/
    meta.bin
    log/
      segment_*.log

raft_snapshots/
  node_1/
    snapshot_<index>/
      data.bin
      __raft_snapshot_meta
```

测试数据默认保存在：

```text
build/tests/raft_test_data/
```

---

## 3. 每个核心 `.cpp` 文件的作用

### 3.1 `src/main.cpp`

程序入口。负责从 `config.txt` 读取节点配置，构造 `NodeConfig` 和 `snapshotConfig`，启动一个 `RaftNode`。

主要职责：

- 解析 `key=value` 格式配置。
- 读取 `node.<id>=host:port` 成员信息。
- 根据命令行参数决定当前启动哪个节点。
- 构造当前节点的 peer 列表。
- 配置 data_dir / snapshot_dir。
- 打印当前节点的 `cluster_size` 和 `quorum`。
- 启动 `RaftNode::Start()`。
- 定期打印 `RaftNode::Describe()`。
- 接收 `SIGINT / SIGTERM` 后停止节点。

典型启动方式：

```bash
./build/raft_demo ./config.txt 1
./build/raft_demo ./config.txt 2
./build/raft_demo ./config.txt 3
```

如果 `config.txt` 只配置 `node.1`，则是单节点 Raft 集群；如果配置 `node.1/node.2/node.3`，则是三节点 Raft 集群。

---

### 3.2 `src/command.cpp`

KV 命令序列化与反序列化。

当前命令格式仍然保持简单字符串格式：

```text
SET|key|value
DEL|key
```

主要职责：

- 将 `Command` 序列化成日志中保存的字符串。
- 从日志字符串反序列化为 `Command`。
- 校验命令是否合法。
- 保持 `SET / DEL` 命令格式稳定。

这个文件不负责 Raft 复制，只负责命令编码。

---

### 3.3 `src/state_machine.cpp`

KV 状态机实现。

主要职责：

- 接收已经 committed 的命令。
- 执行 `SET` / `DEL`。
- 维护内存 KV map。
- 支持 `Get()` 查询。
- 支持 snapshot 保存。
- 支持 snapshot 加载。
- 支持 `DebugString()` 打印当前 KV 状态。

Raft 日志提交后，最终会通过：

```cpp
RaftNode::ApplyCommittedEntries()
  -> state_machine_->Apply(...)
```

把命令应用到这里。

---

### 3.4 `src/raft_node.cpp`

Raft 核心控制器，是当前项目最重要的文件。

主要职责：

- 节点启动和停止。
- gRPC server 初始化。
- peer client 初始化。
- leader election。
- heartbeat 定时器。
- election timer 定时器。
- snapshot 定时器。
- RequestVote 处理。
- AppendEntries 处理。
- InstallSnapshot 处理。
- 本地日志追加。
- 客户端 `Propose()`。
- 多数派复制。
- commit index 推进。
- apply committed logs。
- snapshot 创建与加载。
- 持久化 raft state。
- 重启恢复。
- 维护 `next_index_ / match_index_`。
- 管理每个 follower 的 `Replicator`。

这个文件保留 Raft 节点级逻辑；单个 follower 的复制细节已经拆到 `replicator.cpp`。

---

### 3.5 `src/replicator.cpp`

单个 follower 的复制状态机。

每个 follower 对应一个 `Replicator`。leader 不再直接把所有复制细节堆在 `RaftNode` 中，而是通过 `Replicator` 管理：

- AppendEntries 构造。
- 批量日志复制。
- heartbeat / probe。
- `next_index` 快速回退。
- `match_index` 更新。
- 落后到 snapshot 边界前时触发 InstallSnapshot。
- InstallSnapshot 期间暂停普通 AppendEntries。
- 单 follower in-flight 限制。
- RPC 失败指数退避。
- 复制成功后重置退避。

核心入口：

```cpp
Replicator::ReplicateOnce(...)
```

由 `RaftNode::SendHeartbeats()` 和 `RaftNode::ReplicateLogEntryToMajority()` 调用。

---

### 3.6 `src/raft_service_impl.cpp`

Raft gRPC 服务实现层。

主要职责：

- 接收远端 `RequestVote`。
- 接收远端 `AppendEntries`。
- 接收远端 `InstallSnapshot`。
- 将 RPC 请求转发给 `RaftNode` 的对应处理函数。

典型调用：

```cpp
RaftServiceImpl::RequestVote(...)
  -> RaftNode::OnRequestVote(...)

RaftServiceImpl::AppendEntries(...)
  -> RaftNode::OnAppendEntries(...)

RaftServiceImpl::InstallSnapshot(...)
  -> RaftNode::OnInstallSnapshot(...)
```

这个文件只做 RPC service glue，不直接修改 Raft 状态。

---

### 3.7 `src/raft_storage.cpp`

Raft 持久化存储实现。

当前已经从单个 `raft_state.bin` 升级为：

```text
data_dir/
  meta.bin
  log/
    segment_00000000000000000001.log
    segment_00000000000000000513.log
```

主要职责：

- 保存 `current_term`。
- 保存 `voted_for`。
- 保存 `commit_index`。
- 保存 `last_applied`。
- 分段保存 Raft log records。
- 每条日志保存 header、index、term、size、checksum、data。
- 启动时扫描 segment 文件恢复日志。
- 校验日志 checksum。
- snapshot compaction 后删除不再需要的旧 segment。
- 保证未被 snapshot 覆盖的日志不会被错误删除。

注意：`raft_storage.cpp` 只保存 Raft 日志和元信息，不保存状态机 KV 数据。状态机数据靠 snapshot 或 committed log replay 恢复。

---

### 3.8 `src/snapshot_storage.cpp`

snapshot 文件存储实现。

当前 snapshot 保存为目录式结构：

```text
raft_snapshots/node_1/
  snapshot_00000000000000000120/
    data.bin
    __raft_snapshot_meta
```

主要职责：

- 保存状态机 snapshot 文件。
- 写入 `__raft_snapshot_meta`。
- 记录 `last_included_index`。
- 记录 `last_included_term`。
- 记录 snapshot 数据 checksum。
- 列出所有合法 snapshot。
- 忽略损坏 snapshot。
- 最新 snapshot 损坏时回退到旧 snapshot。
- 自动清理超过保留数量的旧 snapshot。

当前版本按调试要求：不使用 `temp_*` 目录，而是直接写入最终 `snapshot_<index>/` 目录。加载时通过 meta 和 checksum 避免加载损坏快照。

---

### 3.9 `src/min_heap_timer.cpp`

最小堆定时器。

主要职责：

- 支持延迟任务调度。
- 支持取消任务。
- 被 Raft 用于 election timeout、heartbeat timeout、snapshot timeout。
- 内部通过最小堆维护最近到期任务。

Raft 中主要使用位置：

```cpp
ResetElectionTimerLocked()
ResetHeartbeatTimerLocked()
ResetSnapshotTimerLocked()
```

---

### 3.10 `src/thread_pool.cpp`

线程池实现。

主要职责：

- 执行异步任务。
- 用于 RPC 并发发送。
- 支持 Stop。
- Stop 后拒绝新任务。
- 已提交任务在退出前尽量执行完成。

Raft 中用于：

```cpp
RequestVoteRpc
AppendEntriesRpc
InstallSnapshotRpc
Replicator
```

---

## 4. 测试 `.cpp` 文件作用

### 4.1 `tests/test_command.cpp`

测试命令序列化和反序列化。

覆盖：

- `SET` 命令序列化。
- `DEL` 命令序列化。
- 空 key 非法。
- 未知命令非法。
- 错误输入反序列化失败。

---

### 4.2 `tests/test_state_machine.cpp`

测试 KV 状态机。

覆盖：

- `SET` 后可以读回。
- `SET` 覆盖旧值。
- `DEL` 删除已有 key。
- 删除不存在 key 仍然返回成功。
- 非法命令 apply 失败。
- debug string 按 key 排序输出。

---

### 4.3 `tests/test_min_heap_timer.cpp`

测试定时器。

覆盖：

- 定时任务能执行。
- 取消任务不会执行。
- Stop 会清理 pending tasks。

---

### 4.4 `tests/test_thread_pool.cpp`

测试线程池。

覆盖：

- 提交任务会执行。
- Stop 后不再接收新任务。
- Stop 前队列中的任务能完成。

---

### 4.5 `tests/test_raft_election.cpp`

测试基础选举。

覆盖：

- 三节点集群能选出唯一 leader。
- follower 在 leader 存在时拒绝客户端直接 propose。

---

### 4.6 `tests/test_raft_log_replication.cpp`

测试日志复制。

覆盖：

- leader propose 后日志复制到所有节点。
- 多条顺序日志在集群内保持一致。

---

### 4.7 `tests/test_raft_commit_apply.cpp`

测试 commit 和 apply。

覆盖：

- propose 成功后 commit_index / last_applied 推进。
- delete 命令能复制并应用到所有节点。

---

### 4.8 `tests/persistence_test.cpp`

测试纯日志持久化恢复。

覆盖：

- 整个集群停止后重启，依靠 segment log replay 恢复 KV。
- follower 停止后重启，能追上停机期间提交的日志。

注意：这个测试禁用 snapshot，专门验证纯 log 恢复路径。

---

### 4.9 `tests/snapshot_test.cpp`

测试基础 snapshot 保存和加载。

覆盖：

- snapshot 文件生成。
- 重启后从 snapshot 恢复状态机。

---

### 4.10 `tests/raft_integration_test.cpp`

集成测试。

覆盖：

- 三节点选主。
- set/delete 命令复制。
- leader 停止后重新选主。
- 日志达到阈值后生成 snapshot。

---

### 4.11 `tests/test_raft_snapshot_catchup.cpp`

测试 snapshot 追赶。

覆盖：

- follower 落后很多时通过批量 AppendEntries 追赶。
- leader compact 后 follower 通过 InstallSnapshot 追赶。
- follower 安装 snapshot 后继续复制后续日志。

---

### 4.12 `tests/test_raft_snapshot_restart.cpp`

测试 snapshot 与重启恢复组合。

覆盖：

- follower 安装 snapshot 后重启仍能保留状态。
- leader compact 后重启仍能恢复 snapshot 状态。
- 全集群 snapshot 后重启还能继续写。
- snapshot + snapshot 后 tail logs 能完整恢复。

---

### 4.13 `tests/test_raft_snapshot_diagnosis.cpp`

诊断测试。

覆盖：

- 单节点重启时加载 snapshot 后 replay tail logs。
- compacted cluster 重启 leader 后仍能继续复制新日志。
- 失败时打印每个节点 `Describe()`，用于定位恢复路径还是追赶路径问题。

---

### 4.14 `tests/test_raft_segment_storage.cpp`

测试 segment log storage。

覆盖：

- 多个 segment 文件写入。
- segment log 重新加载。
- snapshot compaction 后自动删除旧 segment。
- 真实 Raft 集群生成 segment log 和 snapshot 文件。

---

### 4.15 `tests/test_snapshot_storage_reliability.cpp`

测试 snapshot storage 可靠性。

覆盖：

- snapshot 保存成目录结构。
- 不使用 temp snapshot 目录。
- 最新 snapshot 损坏时回退加载旧 snapshot。
- 超过最大保留数量时自动清理旧 snapshot。

---

### 4.16 `tests/test_raft_replicator_behavior.cpp`

测试 Replicator 行为。

覆盖：

- 慢 follower 不阻塞多数派提交。
- follower 落后后能追上。
- follower 追赶期间 leader 仍能继续接受和提交新日志。

---

## 5. 核心数据结构

### 5.1 `NodeConfig`

定义单个 Raft 节点配置：

```cpp
node_id
address
peers
election_timeout_min
election_timeout_max
heartbeat_interval
rpc_deadline
data_dir
```

### 5.2 `snapshotConfig`

定义 snapshot 行为：

```cpp
enabled
snapshot_dir
log_threshold
snapshot_interval
max_snapshot_count
load_on_startup
file_prefix
```

### 5.3 `LogRecord`

内存中的 Raft 日志项：

```cpp
index
term
command
```

### 5.4 `PersistentRaftState`

磁盘持久化状态：

```cpp
current_term
voted_for
commit_index
last_applied
log
```

### 5.5 `SnapshotMeta`

snapshot 元信息：

```cpp
snapshot_path
meta_path
snapshot_dir
last_included_index
last_included_term
created_unix_ms
data_checksum
```

---

## 6. 关键流程说明

### 6.1 节点启动流程

核心函数链：

```text
main.cpp
  -> LoadKeyValueConfig()
  -> BuildNodeConfig()
  -> BuildSnapshotConfig()
  -> std::make_shared<RaftNode>(...)
  -> RaftNode::RaftNode(...)
       -> CreateFileRaftStorage()
       -> CreateFileSnapshotStorage()
       -> storage_->Load()
       -> LoadLatestSnapshotOnStartup()
       -> ApplyCommittedEntries()
  -> RaftNode::Start()
       -> InitClients()
       -> InitServer()
       -> ResetElectionTimerLocked()
       -> ResetSnapshotTimerLocked()
       -> StartSnapshotWorker()
```

说明：

1. `main.cpp` 从配置文件读取当前节点 ID 和所有成员地址。
2. `RaftNode` 构造时先加载本地持久化 Raft state。
3. 如果开启 snapshot，则尝试加载最新合法 snapshot。
4. 加载 snapshot 后，从 `last_applied_ + 1` 到 `commit_index_` replay 已提交日志。
5. `Start()` 初始化 peer client 和 gRPC server。
6. follower / candidate 启动 election timer。
7. leader 不会注册 election timer。

---

### 6.2 选举流程

核心函数链：

```text
ResetElectionTimerLocked()
  -> scheduler_.ScheduleAfter(...)
  -> OnElectionTimeout(timer_generation)
  -> StartElection()
       -> current_term_ += 1
       -> voted_for_ = self
       -> RequestVoteRpc(peer)
       -> OnElectionWon(term)
       -> BecomeLeaderLocked()
       -> ProposeNoOpEntry()
```

关键点：

- 每一轮新选举都会增加 term。
- 三节点配置只启动一个节点时，只能拿到 1 票，不够 quorum=2，因此会一直 Candidate。
- 单节点配置 `cluster_size=1, quorum=1` 时，可以自己选自己为 leader。
- election timer 使用 `election_timer_generation_` 防止旧 timer 回调在 leader 状态下重新触发选举。
- leader 状态下不会注册 election timer。

---

### 6.3 RequestVote 处理流程

核心函数链：

```text
RaftServiceImpl::RequestVote()
  -> RaftNode::OnRequestVote()
       -> request.term < current_term_ ? reject
       -> request.term > current_term_ ? BecomeFollowerLocked()
       -> IsCandidateLogUpToDateLocked()
       -> voted_for_ 检查
       -> grant / reject
       -> PersistStateLocked()
       -> ResetElectionTimerLocked()
```

投票条件：

- candidate term 不能小于本地 term。
- candidate 日志至少和本地一样新。
- 当前 term 没投过其他 candidate，或者已经投给同一个 candidate。
- grant vote 后重置 election timer。

---

### 6.4 leader 当选流程

核心函数链：

```text
OnElectionWon(term)
  -> BecomeLeaderLocked()
       -> role_ = Leader
       -> leader_id_ = self
       -> CancelElectionTimerLocked()
       -> 初始化 next_index_
       -> 初始化 match_index_
       -> 创建 Replicator
       -> ResetHeartbeatTimerLocked()
  -> ProposeNoOpEntry()
```

说明：

- 成为 leader 后取消 election timer。
- 初始化每个 peer 的复制进度。
- leader 追加一条 no-op 日志，用于确认当前 term 的领导权。
- no-op 也会走正常日志复制和 commit 流程。

---

### 6.5 客户端写入流程

核心函数链：

```text
RaftNode::Propose(command)
  -> ValidateCommandUnlocked()
  -> AppendLocalLogUnlocked(command_data)
  -> PersistStateLocked()
  -> ReplicateLogEntryToMajority(log_index)
  -> AdvanceCommitIndexUnlocked()
  -> ApplyCommittedEntries()
```

说明：

1. 只有 leader 接收 propose。
2. 命令序列化后追加到本地 log。
3. 本地 log 持久化到 segment storage。
4. leader 尝试复制到多数派。
5. 多数派成功后推进 commit_index。
6. committed entry apply 到状态机。
7. apply 后再次持久化 commit/apply 边界。

---

### 6.6 AppendEntries 发送流程

核心函数链：

```text
RaftNode::SendHeartbeats()
  -> Replicator::ReplicateOnce(term, target_index=0)

RaftNode::ReplicateLogEntryToMajority(log_index)
  -> Replicator::ReplicateOnce(term, target_index=log_index)
       -> BuildAppendEntriesRequest()
       -> RaftNode::AppendEntriesRpc()
       -> HandleAppendEntriesResponse()
```

Replicator 负责：

- 根据 `next_index` 构造 `prev_log_index / prev_log_term`。
- 最多发送一批日志。
- 空日志时作为 heartbeat/probe。
- 控制单 peer in-flight。
- RPC 失败后退避。
- 成功后推进 `match_index` 和 `next_index`。
- 失败后根据 follower `last_log_index` 快速回退 `next_index`。

---

### 6.7 AppendEntries 接收流程

核心函数链：

```text
RaftServiceImpl::AppendEntries()
  -> RaftNode::OnAppendEntries()
       -> term 检查
       -> BecomeFollowerLocked() if higher term
       -> prev_log_index / prev_log_term 检查
       -> 冲突日志截断
       -> 追加 leader entries
       -> 更新 commit_index
       -> PersistStateLocked()
       -> ApplyCommittedEntries()
       -> response.success / last_log_index
```

follower 处理规则：

- leader term 小于本地 term，拒绝。
- leader term 大于本地 term，转 follower。
- `prev_log_index / prev_log_term` 不匹配，拒绝并返回本地 `last_log_index`。
- 发现冲突日志时，截断本地冲突部分。
- 追加 leader 发送的新日志。
- 根据 leader commit 更新本地 commit_index。
- apply committed entries。

---

### 6.8 commit index 推进流程

核心函数链：

```text
Replicator::HandleAppendEntriesResponse()
  -> 更新 match_index_[peer]
  -> RaftNode::AdvanceCommitIndexUnlocked()
       -> 统计每个 candidate index 的 match 数量
       -> 达到 quorum
       -> 只提交当前 term 的日志
       -> commit_index_ 前进
```

关键点：

- commit 必须达到多数派。
- leader 只通过当前 term 的日志推进 commit，避免旧 term 日志错误提交。
- commit 后通过 `ApplyCommittedEntries()` 应用到状态机。

---

### 6.9 apply committed logs 流程

核心函数链：

```text
ApplyCommittedEntries()
  -> while last_applied_ < commit_index_
       -> 找到 LogRecord
       -> 反序列化 Command
       -> state_machine_->Apply()
       -> last_applied_++
       -> PersistStateLocked()
```

说明：

- no-op 日志不会改变 KV。
- 普通 `SET / DEL` 命令应用到 `KvStateMachine`。
- apply 后更新 `last_applied_`。
- `last_applied_` 会持久化，用于恢复提交边界。
- 重启时状态机不是直接从 `last_applied_` 恢复，而是从 snapshot 或 0 开始 replay 已提交日志。

---

### 6.10 snapshot 保存流程

核心函数链：

```text
ResetSnapshotTimerLocked()
  -> OnSnapshotTimer()
  -> MaybeScheduleSnapshotLocked(force_by_timer=true)
  -> SnapshotWorkerLoop()
       -> state_machine_->SaveSnapshot(temp_file)
       -> snapshot_storage_->SaveSnapshotFile(...)
       -> CompactLogPrefixLocked(last_included_index, term)
       -> PersistStateLocked()
       -> snapshot_storage_->PruneSnapshots(max_snapshot_count)
```

说明：

- snapshot 覆盖 `last_applied_` 之前的状态。
- snapshot 保存成目录：
  `snapshot_<index>/data.bin + __raft_snapshot_meta`
- 保存后裁剪内存日志前缀。
- 持久化时 segment storage 自动删除不再需要的旧 segment。
- snapshot storage 自动清理超过保留数量的旧 snapshot。

---

### 6.11 snapshot 启动加载流程

核心函数链：

```text
RaftNode::RaftNode(...)
  -> storage_->Load()
  -> LoadLatestSnapshotOnStartup()
       -> snapshot_storage_->LoadLatestValidSnapshot()
       -> state_machine_->LoadSnapshot()
       -> last_snapshot_index_ = meta.last_included_index
       -> last_snapshot_term_ = meta.last_included_term
       -> last_applied_ = last_snapshot_index_
       -> commit_index_ = max(commit_index_, last_snapshot_index_)
       -> CompactLogPrefixLocked(...)
  -> ApplyCommittedEntries()
```

说明：

- 启动时先加载 Raft log/meta。
- 再加载最新合法 snapshot。
- 如果最新 snapshot 损坏，则回退旧 snapshot。
- 加载 snapshot 后 replay snapshot 之后的 committed tail logs。
- 这保证 `snapshot + tail logs` 能完整恢复状态机。

---

### 6.12 纯 log 重启恢复流程

如果没有 snapshot，状态机从空开始：

```text
RaftNode::RaftNode(...)
  -> storage_->Load()
       -> current_term_
       -> voted_for_
       -> commit_index_
       -> persisted_last_applied
       -> log_
  -> last_applied_ = 0
  -> ApplyCommittedEntries()
       -> replay 1..commit_index_
```

重要点：

- 不能把运行时 `last_applied_` 直接设置为 `persisted_last_applied`。
- 因为状态机 KV 没有单独持久化，必须 replay log。
- `persisted_last_applied` 只用于确定已提交边界，不用于跳过状态机 replay。

---

### 6.13 落后 follower 追赶流程

正常批量追赶：

```text
leader Replicator::ReplicateOnce()
  -> AppendEntries(prev_log_index, prev_log_term, entries)
  -> follower OnAppendEntries()
  -> success
  -> leader 更新 match_index / next_index
  -> 继续下一批
```

日志不匹配：

```text
follower 返回 success=false + last_log_index
leader 根据 follower last_log_index + 1 调整 next_index
继续 probe / append
```

日志已经被 snapshot 裁剪：

```text
Replicator::BuildAppendEntriesRequest()
  -> 发现 next_index <= last_snapshot_index 或 prev_log term 不存在
  -> should_install_snapshot=true
  -> RaftNode::SendInstallSnapshotToPeer()
```

---

### 6.14 InstallSnapshot 流程

leader 侧：

```text
Replicator::ReplicateOnce()
  -> should_install_snapshot
  -> RaftNode::SendInstallSnapshotToPeer(peer_id, term)
       -> 读取最新 snapshot
       -> 构造 InstallSnapshotRequest
       -> InstallSnapshotRpc()
       -> 成功后更新 next_index / match_index
```

follower 侧：

```text
RaftServiceImpl::InstallSnapshot()
  -> RaftNode::OnInstallSnapshot()
       -> term 检查
       -> BecomeFollowerLocked()
       -> 保存 snapshot data
       -> state_machine_->LoadSnapshot()
       -> last_snapshot_index_ / last_snapshot_term_
       -> commit_index_ / last_applied_
       -> CompactLogPrefixLocked()
       -> PersistStateLocked()
```

安装完成后：

```text
leader 继续 AppendEntries 复制 snapshot 之后的日志
```

---

### 6.15 脑裂 / 网络分区下的行为

Raft 处理脑裂依赖三个机制：

```text
多数派提交
term 比较
日志冲突回滚
```

三节点配置只启动一个节点：

```text
cluster_size=3
quorum=2
只有 1 票
不能成为 leader
会一直 Candidate
每一轮新选举 term + 1
```

单节点配置只包含 `node.1`：

```text
cluster_size=1
quorum=1
可以自己选自己为 leader
当选后 term 不应持续增长
```

如果旧 leader 被隔离在少数派：

```text
旧 leader 可能短时间认为自己是 leader
但拿不到多数派，不能 commit
多数派分区会选出新 leader
网络恢复后旧 leader 收到更高 term，退回 follower
未提交冲突日志被新 leader 覆盖
```

注意：未来实现 `Get` 时不能直接从旧 leader 本地读，否则有 stale read 风险。需要 ReadIndex 或 no-op read。

---

## 7. 构建与运行

### 7.1 低并发构建

如果机器 CPU / 内存容易被 Ninja 打满，建议使用：

```bash
cmake -S . -B build \
  -DRAFT_BUILD_JOBS=1 \
  -DRAFT_LINK_JOBS=1 \
  -DRAFT_PROTO_JOBS=1

cmake --build build --parallel 1
```

### 7.2 运行三节点集群

`config.txt` 示例：

```text
node_id=1

node.1=127.0.0.1:50051
node.2=127.0.0.1:50052
node.3=127.0.0.1:50053

data_root=./raft_data
snapshot_root=./raft_snapshots

election_timeout_min_ms=800
election_timeout_max_ms=1600
heartbeat_interval_ms=150
rpc_deadline_ms=500

snapshot_enabled=true
snapshot_log_threshold=30
snapshot_interval_ms=600000
snapshot_max_count=5
snapshot_load_on_startup=true
snapshot_file_prefix=snapshot

describe_interval_ms=5000
```

启动：

```bash
./build/raft_demo ./config.txt 1
./build/raft_demo ./config.txt 2
./build/raft_demo ./config.txt 3
```

### 7.3 运行单节点集群

如果只想测试单节点模式，配置中只保留：

```text
node_id=1
node.1=127.0.0.1:50051
```

此时应看到：

```text
cluster_size=1, quorum=1
start election, term=1
won election, become leader, term=1
```

后续 term 不应继续增长。

---

## 8. 测试

查看全部测试：

```bash
ctest --test-dir build -N
```

跑全部测试：

```bash
ctest --test-dir build --output-on-failure -j1
```

按类别运行：

```bash
ctest --test-dir build -R CommandTest --output-on-failure -j1
ctest --test-dir build -R KvStateMachineTest --output-on-failure -j1
ctest --test-dir build -R RaftElectionTest --output-on-failure -j1
ctest --test-dir build -R RaftLogReplicationTest --output-on-failure -j1
ctest --test-dir build -R RaftCommitApplyTest --output-on-failure -j1
ctest --test-dir build -R PersistenceTest --output-on-failure -j1
ctest --test-dir build -R RaftSnapshot --output-on-failure -j1
ctest --test-dir build -R RaftSegmentStorageTest --output-on-failure -j1
ctest --test-dir build -R SnapshotStorageReliabilityTest --output-on-failure -j1
ctest --test-dir build -R RaftReplicatorBehaviorTest --output-on-failure -j1
```

保留测试数据：

```bash
RAFT_TEST_KEEP_DATA=1 ctest --test-dir build -R RaftSnapshot --output-on-failure -j1
```

测试数据位置：

```text
build/tests/raft_test_data/
```

---

## 9. 持久化文件说明

### 9.1 Raft data

```text
raft_data/node_1/
  meta.bin
  log/
    segment_00000000000000000001.log
    segment_00000000000000000513.log
```

`meta.bin` 保存：

```text
current_term
voted_for
commit_index
last_applied
first_log_index
last_log_index
log_count
```

`segment_*.log` 保存：

```text
record header
record data
checksum
```

### 9.2 Snapshot

```text
raft_snapshots/node_1/
  snapshot_00000000000000000120/
    data.bin
    __raft_snapshot_meta
```

`data.bin` 是状态机 snapshot 数据。

`__raft_snapshot_meta` 保存：

```text
last_included_index
last_included_term
created_unix_ms
data_file_name
data_checksum
```

---

## 10. 后续开发建议

当前 Raft 内核已经基本完成实验级能力。后续更适合转向应用层：

### 10.1 对外 KV Service

新增：

```text
KvService.Put
KvService.Get
KvService.Delete
```

第一版：

- Put/Delete 只允许 leader 处理。
- follower 返回 leader_id。
- Get 第一版可以只允许 leader 读。
- 后续补 ReadIndex 保证线性一致读。

### 10.2 File Storage

不要把大文件内容写入 Raft log。推荐：

```text
Raft 保存 file metadata
StorageNode 保存 chunk data
```

新增：

```text
FileStorage
StorageService.StoreChunk
StorageService.ReadChunk
StorageService.DeleteChunk
FileService.PutFile
FileService.GetFile
```

### 10.3 Meta / Manager

多 RaftGroup 后再引入 Meta：

```text
object/key -> slot -> group
group -> leader/node list
```

Meta 只管路由和状态，不传输用户数据。

---

## 11. 当前关键约束

- 不要把大文件内容直接放入 Raft log。
- Raft log 只适合复制小命令或元数据。
- 写请求必须多数派 commit 后才能返回成功。
- 读请求未来需要 ReadIndex / no-op read，避免旧 leader stale read。
- membership 不能根据“当前连得上的节点数”动态变化。
- 三节点配置只启动一个节点时，不应该成为 leader。
- 单节点配置可以自己成为 leader。
- snapshot 删除只删除已经被覆盖的旧日志或旧 snapshot，不要删除仍可能用于恢复/追赶的日志。
