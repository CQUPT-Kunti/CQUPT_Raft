# CQUPT_Raft 项目 AI 上下文文档

> 用途：  
> 这份文档用于在新的 AI 聊天中快速恢复项目上下文。后续如果需要继续开发、排查 bug、生成代码，可以先把这份文档上传给 AI，并说明“基于这份上下文继续，不要重新推翻已有设计”。

---

## 1. 项目基本信息

项目是一个基于 **C++20 + gRPC + Protobuf + GoogleTest** 的 Raft KV 存储内核，项目名类似：

```text
CQUPT_Raft / raft_grpc_cpp
```

当前阶段目标是：

```text
先完成单 RaftGroup 的 Raft 内核稳定性，然后再往 KV Service / File Storage / Meta 管理方向扩展。
```

当前不急着做：

```text
1. 多集群 Meta / Manager
2. 文件对象存储 StorageNode
3. 外部复杂客户端
4. 动态成员变更
5. 工业级异步 WAL
```

目前主要关注的是：

```text
Raft 选举、日志复制、提交、持久化、快照、落后追赶、重启恢复、测试稳定性。
```

---

## 2. 用户对代码修改的长期要求

后续继续开发时必须遵守这些要求：

```text
1. 尽量保持已有变量名、函数名和整体结构，不要无意义大改。
2. 没有特殊必要，不要推翻已有设计。
3. 不修改 command 序列化格式。
   现有格式是：
     SET|key|value
     DEL|key
4. 要保持跨平台，尽量只用 C++20 标准库，不使用 Linux-only API。
5. 测试统一使用 GoogleTest。
6. 测试产生的数据要放到 build/tests/raft_test_data 下，方便查看。
7. 不要把测试数据放到 /tmp。
8. 构建和测试要限制并发，机器 CPU/内存容易被打满。
9. 当前优先保证 Raft 内核正确，再做应用层。
10. 如果生成代码，直接返回需要替换的文件。
```

---

## 3. 当前核心目录结构

当前项目大致结构：

```text
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

proto/
  raft.proto

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

---

## 4. 每个核心 cpp 文件作用

### `src/main.cpp`

程序入口。

当前版本支持：

```text
./build/raft_demo ./config.txt 1
./build/raft_demo ./config.txt 2
./build/raft_demo ./config.txt 3
```

主要负责：

```text
1. 读取 config.txt。
2. 根据 node.<id>=addr 构造集群成员。
3. 根据命令行参数决定当前启动哪个 node。
4. 构造 NodeConfig。
5. 构造 snapshotConfig。
6. 打印 cluster_size 和 quorum。
7. 启动 RaftNode。
8. 定期打印 Describe()。
9. Ctrl+C 后停止节点。
```

---

### `src/command.cpp`

KV 命令序列化和反序列化。

当前格式：

```text
SET|key|value
DEL|key
```

要求：**后续不要轻易修改这个格式。**

---

### `src/state_machine.cpp`

KV 状态机。

负责：

```text
1. Apply SET。
2. Apply DEL。
3. Get key。
4. DebugString。
5. 保存 snapshot。
6. 加载 snapshot。
```

注意：状态机本身不是每次 apply 都单独持久化。重启时依靠：

```text
snapshot + committed log replay
```

恢复 KV 状态。

---

### `src/raft_node.cpp`

Raft 核心节点逻辑。

负责：

```text
1. Start / Stop。
2. gRPC server 启动。
3. peer client 初始化。
4. election timer。
5. heartbeat timer。
6. snapshot timer。
7. RequestVote 处理。
8. AppendEntries 处理。
9. InstallSnapshot 处理。
10. Propose。
11. 本地日志 append。
12. commit_index 推进。
13. ApplyCommittedEntries。
14. snapshot 创建。
15. snapshot 加载。
16. 持久化恢复。
17. 管理 Replicator。
```

这是当前最核心的文件。

---

### `src/replicator.cpp`

单个 follower 的复制状态机。

每个 follower 一个 `Replicator`。

负责：

```text
1. 构造 AppendEntries。
2. 批量复制日志。
3. heartbeat / probe。
4. next_index / match_index 维护。
5. AppendEntries 失败时根据 follower last_log_index 快速回退。
6. follower 落后到 snapshot 边界前时触发 InstallSnapshot。
7. InstallSnapshot 期间暂停普通 AppendEntries。
8. 单 follower in-flight 限制。
9. RPC 失败指数退避。
10. snapshot 安装完成后继续追后续日志。
```

这是从 `RaftNode` 里拆出来的 follower 复制逻辑。

---

### `src/raft_service_impl.cpp`

Raft gRPC 服务 glue 层。

负责把 RPC 转发给 `RaftNode`：

```text
RequestVote RPC      -> RaftNode::OnRequestVote
AppendEntries RPC   -> RaftNode::OnAppendEntries
InstallSnapshot RPC -> RaftNode::OnInstallSnapshot
```

---

### `src/raft_storage.cpp`

Raft 持久化层。

当前已经从单文件 `raft_state.bin` 改成：

```text
raft_data/node_1/
  meta.bin
  log/
    segment_*.log
```

负责：

```text
1. 保存 current_term。
2. 保存 voted_for。
3. 保存 commit_index。
4. 保存 last_applied。
5. 保存 Raft log segment。
6. 每条 log entry 带 index、term、size、checksum。
7. 启动时扫描 segment 恢复日志。
8. snapshot compaction 后删除旧 segment。
```

注意：不要无条件删除未被 snapshot 覆盖的日志，否则 follower 追赶和重启恢复会出问题。

---

### `src/snapshot_storage.cpp`

snapshot 文件存储。

当前结构：

```text
raft_snapshots/node_1/
  snapshot_<index>/
    data.bin
    __raft_snapshot_meta
```

负责：

```text
1. 保存 snapshot data。
2. 保存 snapshot meta。
3. 校验 checksum。
4. 加载最新合法 snapshot。
5. 最新 snapshot 损坏时回退旧 snapshot。
6. 自动清理超过保留数量的旧 snapshot。
```

用户明确要求：**不要使用 temp_* snapshot 目录**。当前是直接写 `snapshot_<index>/`。

---

### `src/min_heap_timer.cpp`

最小堆定时器。

用于：

```text
1. election timeout。
2. heartbeat timeout。
3. snapshot timeout。
```

---

### `src/thread_pool.cpp`

线程池。

用于异步执行 RPC、复制任务等。

---

## 5. 当前已经完成的重要功能

### 5.1 选举

已实现：

```text
1. Follower election timeout 后变 Candidate。
2. Candidate current_term += 1。
3. Candidate 给自己投票。
4. Candidate 发送 RequestVote。
5. 拿到多数派后成为 Leader。
6. Leader 当选后追加 no-op 日志。
7. Leader 收到更高 term RPC 后退回 Follower。
```

已经修复的问题：

```text
leader 状态下旧 election timer 回调可能继续触发选举，导致单节点 term 持续增长。
```

修复方式：

```text
1. 增加 election_timer_generation_。
2. 每次 cancel/reset election timer 都递增 generation。
3. timer callback 触发时检查 generation 是否仍然有效。
4. leader 状态下不再注册 election timer。
5. BecomeLeader 时取消 election timer。
```

验证结果：

```text
单节点配置：
  cluster_size=1, quorum=1
  只选举一次，term=1 后不再持续增长。

三节点配置只启动一个节点：
  cluster_size=3, quorum=2
  节点一直 Candidate，每一轮 election timeout term +1。
  这是正常 Raft 行为。
```

---

### 5.2 日志复制

已实现：

```text
1. Leader 本地 append log。
2. Leader 持久化 log。
3. Leader 通过 Replicator 给 follower 发 AppendEntries。
4. Follower 检查 prev_log_index / prev_log_term。
5. Follower 追加日志。
6. Leader 更新 match_index / next_index。
7. Leader 多数派成功后 commit。
8. Follower 根据 leader_commit 更新 commit_index。
```

已经优化：

```text
1. AppendEntries 支持批量发送。
2. follower 落后时用 last_log_index 快速回退 next_index。
3. 不再一个一个 index 慢慢回退。
```

---

### 5.3 提交与 apply

已实现：

```text
1. 只有多数派复制成功才能 commit。
2. leader 推进 commit_index。
3. follower 根据 leader_commit 推进 commit_index。
4. committed log 通过 ApplyCommittedEntries 应用到 state_machine。
```

注意：

```text
no-op 日志会推进 commit/apply，但不会改变 KV。
```

---

### 5.4 重启恢复

已实现：

```text
1. 从 meta.bin 恢复 current_term / voted_for / commit_index / persisted_last_applied。
2. 从 segment log 恢复日志。
3. 加载 snapshot。
4. replay snapshot 之后的 committed tail logs。
5. 如果没有 snapshot，则从 0 开始 replay committed logs。
```

最近修复的重要问题：

```text
之前启动时直接把 persistent_state.last_applied 赋给运行时 last_applied_。
这会导致 commit_index == last_applied_，从而跳过 committed log replay。
结果是纯 log 重启后 KV 状态为空。
```

正确逻辑：

```text
1. persistent_state.last_applied 只表示持久化提交边界。
2. 状态机需要从 snapshot 或 0 开始恢复。
3. 如果没有 snapshot，运行时 last_applied_ 应该从 0 开始。
4. 然后 replay 1..commit_index_。
5. 如果有 snapshot，运行时 last_applied_ 从 snapshot index 开始。
6. 然后 replay snapshot_index+1..commit_index_。
```

对应修复：

```cpp
commit_index_ = max(persistent_state.commit_index,
                    persistent_state.last_applied);
last_applied_ = 0;
```

snapshot 加载成功后再把 `last_applied_` 设置成 snapshot index。

---

### 5.5 Snapshot

已实现：

```text
1. 状态机保存 snapshot。
2. snapshot 目录式保存。
3. snapshot meta 保存 last_included_index / last_included_term / checksum。
4. 启动时加载最新合法 snapshot。
5. 最新 snapshot 损坏时回退旧 snapshot。
6. snapshot 后裁剪日志。
7. snapshot 后自动删除旧 segment。
8. snapshot 后 tail logs 能继续 replay。
```

---

### 5.6 InstallSnapshot / 落后节点追赶

已实现：

```text
1. follower 落后但 leader 还有日志：批量 AppendEntries 追赶。
2. follower 落后且 leader 已 compact：InstallSnapshot。
3. follower 安装 snapshot 后继续追 snapshot 之后的日志。
4. restarted follower 能通过 snapshot + AppendEntries 追上。
```

---

### 5.7 Replicator

已实现独立 `Replicator`：

```text
RaftNode:
  负责选举、commit、apply、snapshot、持久化

Replicator:
  负责单个 follower 的日志复制、追赶、InstallSnapshot、退避、in-flight
```

---

## 6. 当前测试情况

目前大部分测试通过，包括：

```text
CommandTest
KvStateMachineTest
TimerSchedulerTest
ThreadPoolTest
RaftElectionTest
RaftLogReplicationTest
RaftCommitApplyTest
RaftIntegrationTest
RaftSnapshotRecoveryTest
RaftSnapshotCatchupTest
RaftSnapshotRestartTest
RaftSnapshotDiagnosisTest
RaftSegmentStorageTest
SnapshotStorageReliabilityTest
RaftReplicatorBehaviorTest
```

曾经失败但已定位/修复的测试：

```text
PersistenceTest.FullClusterRestartRecovery
PersistenceTest.RestartedFollowerCatchesUp
```

失败原因：

```text
重启恢复时跳过了 committed log replay，导致 KV 为空或 follower 缺失旧值。
```

修复后应重新运行：

```bash
ctest --test-dir build -R PersistenceTest --output-on-failure -j1
ctest --test-dir build -R RaftSnapshotRestartTest --output-on-failure -j1
ctest --test-dir build -R RaftSnapshotDiagnosisTest --output-on-failure -j1
```

---

## 7. 当前已知行为解释

### 7.1 三节点配置只启动一个节点，term 一直涨

这是正常行为。

原因：

```text
cluster_size=3
quorum=2
只启动 node-1
node-1 只能拿到自己 1 票
不够 quorum
所以一直 Candidate
每一轮新的 election 都 current_term += 1
```

这不是 bug。

---

### 7.2 单节点配置只启动一个节点

如果 config 只包含：

```text
node.1=127.0.0.1:50051
```

那么：

```text
cluster_size=1
quorum=1
node-1 可以自己成为 leader
```

正确现象：

```text
start election, term=1
won election, become leader, term=1
之后 term 不再持续增长
```

---

### 7.3 Raft 如何处理脑裂

写入安全依赖：

```text
多数派提交 + term 比较 + 日志冲突回滚
```

5 节点分区成 2 + 3：

```text
3 节点分区可以选 leader 并提交。
2 节点分区不能提交写入。
网络恢复后，少数派旧 leader 收到更高 term 后退位。
未提交冲突日志被新 leader 覆盖。
```

注意：

```text
未来 Get 不能直接读旧 leader 本地状态机。
需要 ReadIndex 或 no-op read。
```

---

## 8. 构建和测试注意事项

用户机器 CPU/内存容易被打满，所以建议低并发。

### 8.1 CMake 构建

```bash
cmake -S . -B build \
  -DRAFT_BUILD_JOBS=1 \
  -DRAFT_LINK_JOBS=1 \
  -DRAFT_PROTO_JOBS=1

cmake --build build --parallel 1
```

### 8.2 CTest

```bash
ctest --test-dir build --output-on-failure -j1
```

注意：VS Code CMake Tools 可能默认用：

```text
ctest -j12
```

需要在 `.vscode/settings.json` 里设置：

```json
{
    "cmake.ctestArgs": [
        "-j1",
        "--output-on-failure"
    ],
    "cmake.parallelJobs": 1,
    "cmake.buildArgs": [
        "--parallel",
        "1"
    ],
    "cmake.configureArgs": [
        "-DRAFT_BUILD_JOBS=1",
        "-DRAFT_LINK_JOBS=1",
        "-DRAFT_PROTO_JOBS=1"
    ],
    "cmake.environment": {
        "CMAKE_BUILD_PARALLEL_LEVEL": "1",
        "CTEST_PARALLEL_LEVEL": "1"
    }
}
```

不要使用错误 key：

```json
"cmake.testArgs"
```

正确是：

```json
"cmake.ctestArgs"
```

---

## 9. 当前适合的下一步方向

用户希望从 Raft 内核转向应用层。

推荐顺序：

### 9.1 第一阶段：对外 KV Service

新增：

```text
KvService.Put
KvService.Get
KvService.Delete
```

第一版语义：

```text
Put/Delete:
  只能 leader 处理。
  follower 返回 NOT_LEADER + leader_id。
  leader 调用 Propose。

Get:
  第一版可以只允许 leader 读。
  后续必须补 ReadIndex，避免 stale read。
```

---

### 9.2 第二阶段：StorageNode

对象/文件存储不要把大文件直接写入 Raft log。

推荐：

```text
Raft 只保存文件元数据。
StorageNode 保存 chunk 数据。
```

StorageNode 第一版功能：

```text
StoreChunk
ReadChunk
DeleteChunk
HasChunk
```

本地路径：

```text
storage_data/chunks/ab/cd/<chunk_id>.chunk
```

不要每次读 chunk 都查 MySQL。StorageNode 本机根据 `chunk_id` 直接计算路径，或者维护本地索引。

---

### 9.3 第三阶段：FileService

基于 Raft metadata + StorageNode chunk data 实现：

```text
PutFile
GetFile
DeleteFile
```

流程：

```text
PutFile:
  1. 切 chunk
  2. 写 StorageNode
  3. 多副本写成功
  4. Raft Propose 文件元数据
  5. metadata commit 后返回成功

GetFile:
  1. 查 Raft metadata
  2. 找到 chunk_id 和 replicas
  3. 去 StorageNode 读 chunk
  4. 校验 checksum
  5. 拼接返回
```

---

### 9.4 第四阶段：Meta / Manager

多集群时才需要。

职责：

```text
1. 管理 group。
2. 管理 node 状态。
3. 管理 leader 信息。
4. 管理 slot -> group 路由。
5. 告诉客户端某个 key/object 去哪个 group。
```

Meta 不传输用户数据，只做元数据和路由。

第一版可以用：

```text
单 MetaServer + MySQL
```

但注意这是单点。以后可以升级为 Meta Raft 集群。

---

## 10. 对象存储设计相关结论

### 10.1 chunk_id 存哪里

工业上一般分两层：

```text
Meta:
  保存 object -> chunks -> replicas

StorageNode:
  保存 chunk_id -> local path
```

StorageNode 本地不要每次查 MySQL。

推荐：

```text
chunk_id = abcd1234
path = root/chunks/ab/cd/abcd1234.chunk
```

以后需要 GC/统计时可以加本地 RocksDB/SQLite 索引。

---

### 10.2 MD5 和 slot

MD5 可以用于：

```text
1. checksum
2. object_id
3. hash 输入
```

但不建议直接：

```text
target = md5(key) % 上位机数量
```

因为扩容时大量数据会重新映射。

推荐：

```text
slot = hash(key) % 4096
slot -> group
```

这样扩容时只迁移部分 slot。

---

## 11. 重要 commit 记录建议

最近可以合并成一个 commit：

```text
fix(raft): 修复选举定时器与重启日志重放问题

- 为 election timer 增加 generation 校验，忽略已经失效的旧定时器回调
- leader 状态下不再注册 election timer，避免当选后再次触发选举
- BecomeLeader 时取消已有 election timer，修复单节点 leader 任期持续增长问题
- main 支持通过 config.txt 启动指定 node，并打印 cluster_size 和 quorum
- 启动时不再直接将 persisted_last_applied 赋给运行时 last_applied
- 使用 persisted commit_index / last_applied 恢复已提交日志边界
- 状态机从 snapshot index 或 0 开始 replay 已提交日志
- 修复无 snapshot 场景下重启后 KV 状态为空的问题
- 修复 restarted follower 缺失重启前已提交日志的问题
- 验证单节点集群只选举一次且 term 不再持续增长
- 验证 PersistenceTest.FullClusterRestartRecovery 和 RestartedFollowerCatchesUp 通过
```

---

## 12. 给后续 AI 的提示

如果继续开发，请先确认：

```text
1. 当前代码是否已经包含 election_timer_generation_ 修复。
2. 当前代码是否已经包含“重启时 last_applied_ 不直接等于 persisted_last_applied”的修复。
3. PersistenceTest 是否已经重新跑通。
4. tests/CMakeLists.txt 是否注册了全部测试。
5. .vscode/settings.json 是否限制了 ctest 并发。
```

不要轻易重写 Raft 核心。  
如果要继续功能开发，优先做：

```text
1. KvService
2. ReadIndex
3. StorageNode
4. FileService
5. Meta / Manager
```
