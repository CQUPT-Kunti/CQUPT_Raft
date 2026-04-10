# Raft 项目 README（面向当前代码实现的流程说明）

> 这份 README 不是照着论文复述 Raft，而是**按当前这份代码的实现方式**来解释：
> - 每个模块负责什么
> - 选举、投票、日志复制、提交、状态机应用分别是怎么走的
> - 关键变量在每一步分别代表什么
> - `main.cpp` 里的测试到底测到了什么
>
> 阅读建议：先看“整体流程图”和“关键变量对照”，再回头对照 `raft_node.cpp` 看具体函数，会更容易理解。

---

## 1. 项目整体结构

当前代码大致可以分成下面几个部分：

### 1.1 Raft 核心节点
- 文件：`raft_node.h` / `raft_node.cpp`
- 作用：
  - 保存一个节点的核心状态
  - 处理选举超时
  - 发起投票
  - 处理投票请求
  - 发送心跳 / 日志复制请求
  - 处理 `AppendEntries`
  - 管理提交位置 `commit_index_`
  - 把已提交日志应用到状态机

这个类是整个项目的核心。

---

### 1.2 RPC 服务层
- 文件：`raft_service_impl.h` / `raft_service_impl.cpp`
- 作用：
  - 暴露 gRPC 接口
  - 把 `RequestVote` 和 `AppendEntries` 请求转发给 `RaftNode`

这里本身逻辑很薄，主要是把网络入口接进来。

---

### 1.3 命令抽象层
- 文件：`command.h` / `command.cpp`
- 作用：
  - 定义业务命令 `Command`
  - 目前支持：
    - `SET key value`
    - `DEL key`
  - 提供：
    - `IsValid()`：校验命令是否合法
    - `Serialize()`：序列化为字符串，写入日志
    - `Deserialize()`：从日志中的字符串反序列化回命令

也就是说，Raft 日志里真正存的是序列化后的字符串，而不是直接存 `Command` 对象。

---

### 1.4 状态机层
- 文件：`state_machine.h` / `state_machine.cpp`
- 作用：
  - `IStateMachine` 是抽象接口
  - `KvStateMachine` 是当前实现的 KV 状态机
  - 负责把**已经提交的日志**真正执行到内存 KV 上

当前状态机规则很简单：
- `SET`：写入或覆盖
- `DEL`：删除 key

注意：**状态机只能执行已经 commit 的日志**。

---

### 1.5 配置层
- 文件：`config.h`
- 作用：
  - 定义 `PeerConfig`
  - 定义 `NodeConfig`
  - 保存节点地址、peer 列表、选举超时、心跳间隔、RPC 超时等参数

---

### 1.6 定时器和线程池
- 文件：
  - `min_heap_timer.h` / `min_heap_timer.cpp`
  - `thread_pool.h` / `thread_pool.cpp`
- 作用：
  - `TimerScheduler`：负责调度选举超时和心跳定时任务
  - `ThreadPool`：负责并发发送 RPC（投票请求 / 日志复制请求）

---

### 1.7 测试入口
- 文件：`main.cpp`
- 作用：
  - 启动 3 个节点
  - 等待 leader 选出
  - 向 leader 提交一系列命令
  - 打印集群快照，观察日志、提交、应用和 KV 状态

---

## 2. 一个节点内部有哪些核心状态

下面这些变量是理解整个流程的关键。

---

### 2.1 角色相关

#### `role_`
表示当前节点角色：
- `Role::kFollower`
- `Role::kCandidate`
- `Role::kLeader`

这是判断一个节点当前职责的核心变量。

#### `leader_id_`
表示当前已知 leader 的节点 ID。
- follower 收到 leader 心跳后会更新它
- candidate 开始选举时会先把它设为 `-1`
- leader 成功上位后会把它设成自己的 `config_.node_id`

---

### 2.2 任期和投票相关

#### `current_term_`
表示当前节点看到的任期。
- 发起新一轮选举时自增
- 收到更高任期请求时更新为对方的 term

它是 Raft 里最核心的“时代编号”。

#### `voted_for_`
表示当前任期投票投给了谁。
- 初始是 `-1`
- candidate 发起选举时会先投给自己
- follower 在当前 term 内只会投给一个 candidate

---

### 2.3 日志相关

#### `log_`
类型是 `std::vector<LogRecord>`。

每条日志包括：
- `index`
- `term`
- `command`

你当前代码里在构造函数里先塞了一条：
- `LogRecord{0, 0, "bootstrap"}`

所以你的有效业务日志通常从下标 `1` 开始。

#### `LastLogIndexLocked()`
返回当前日志最后一条的索引。

#### `LastLogTermLocked()`
返回当前日志最后一条的任期。

这两个函数会在选举投票时用来比较“谁的日志更新”。

---

### 2.4 提交和应用相关

#### `commit_index_`
表示“已经提交”的最大日志下标。

含义是：
- 这个下标及之前的日志，已经可以认为达成提交条件
- 但**提交了不等于已经执行到状态机**

#### `last_applied_`
表示“已经应用到状态机”的最大日志下标。

因此，任意时刻应该满足：
- `last_applied_ <= commit_index_`

当 `last_applied_ < commit_index_` 时，说明还有日志已经提交，但还没被状态机执行。

---

### 2.5 Leader 复制进度相关

#### `next_index_`
`unordered_map<int, std::uint64_t>`，以 peer 节点 ID 为 key。

含义：
- leader 下一次准备发给某个 follower 的日志起点

如果某个 follower 一直跟得上 leader，`next_index_[peer]` 会越来越大。
如果某次复制失败，leader 会把它往回减，用来回退重试。

#### `match_index_`
`unordered_map<int, std::uint64_t>`，以 peer 节点 ID 为 key。

含义：
- leader 认为某个 follower 已经成功复制到的最大日志下标

这个变量后续非常重要，因为**标准 Raft 的 commit 推进**就是基于它来判断“是否已经复制到多数派”。

---

### 2.6 并发控制相关

#### `mu_`
Raft 核心状态锁。

保护的对象主要包括：
- `role_`
- `current_term_`
- `voted_for_`
- `leader_id_`
- `log_`
- `commit_index_`
- `last_applied_`
- `next_index_`
- `match_index_`

#### `apply_mu_`
状态机应用锁。

它的作用是避免多个线程同时进入 `ApplyCommittedEntries()`，从而重复 apply。

---

## 3. 一次完整运行的总流程

把整个系统从启动到写入，粗略分成下面几步：

1. `main.cpp` 构造 3 个 `RaftNode`
2. 每个节点 `Start()`
3. 节点启动 gRPC 服务，启动定时器，重置选举超时
4. 某个 follower 先超时，触发 `OnElectionTimeout()`
5. 它进入 `StartElection()`，变成 candidate，term 加 1，给自己投票
6. 它向其他节点发 `RequestVote`
7. 多数派同意后，candidate 进入 `OnElectionWon()`，成为 leader
8. leader 进入心跳循环，周期性发送 `AppendEntries`
9. `main.cpp` 找到 leader，向 leader 调用 `Propose()`
10. leader 本地追加日志
11. leader 向 follower 复制日志
12. 日志达到提交条件后推进 `commit_index_`
13. 调用 `ApplyCommittedEntries()`
14. 状态机执行日志，更新 KV
15. follower 收到包含 `leader_commit` 的 `AppendEntries` 后，也会推进本地 `commit_index_` 并 apply
16. 最终所有节点的 `kv` 内容一致

---

## 4. 节点启动流程

### 4.1 `RaftNode::Start()`
这个函数做了几件事：

1. `InitClients()`
   - 为每个 peer 创建 gRPC channel 和 stub

2. `scheduler_.Start()`
   - 启动最小堆定时器线程

3. `InitServer()`
   - 启动本节点的 gRPC server
   - 注册 `RaftServiceImpl`

4. 加锁后 `ResetElectionTimerLocked()`
   - 安排一个随机选举超时任务

所以节点一启动，本质上就已经进入：
- 等请求
- 等超时
- 等选举

的状态了。

---

## 5. 选举流程

### 5.1 选举超时怎么触发

#### `ResetElectionTimerLocked()`
- 先取消旧的选举定时器
- 调 `RandomElectionTimeoutLocked()` 生成一个随机时间
- 定时到期后执行 `OnElectionTimeout()`

#### `RandomElectionTimeoutLocked()`
- 从 `election_timeout_min ~ election_timeout_max` 之间随机选一个时间

这样做的目的是减少多个节点同时发起选举的概率。

---

### 5.2 `OnElectionTimeout()`
逻辑很简单：
- 如果节点已经停止，返回
- 如果当前已经是 leader，返回
- 否则调用 `StartElection()`

也就是说，**只有 follower / candidate 才会因为超时继续发起选举**。

---

### 5.3 `StartElection()`
这是 candidate 发起选举的入口。

进入后主要做这些事：

1. 加锁检查当前节点是否还能发起选举
2. `role_ = Role::kCandidate`
3. `++current_term_`
4. `voted_for_ = config_.node_id`（先给自己投票）
5. `leader_id_ = -1`
6. 记录：
   - `term`
   - `last_log_index`
   - `last_log_term`
7. 重新设置选举定时器
8. 并发向所有 peer 发送 `RequestVote`

这里有两个很关键的局部量：

#### `votes`
初始为 1，因为 candidate 先投给自己。

#### `quorum`
表示多数派门槛。
3 节点情况下就是 2。

---

### 5.4 candidate 发出的 `VoteRequest` 里有什么

`raft.proto` 中定义：
- `term`
- `candidate_id`
- `last_log_index`
- `last_log_term`

含义是：
- 我现在在哪个 term
- 我是谁
- 我当前日志最新到哪里

这样 follower 才能决定要不要投票给它。

---

### 5.5 follower 如何处理投票请求

入口函数：`RaftNode::OnRequestVote()`

处理逻辑分为几步：

#### 第一步：默认拒绝
```cpp
response->set_vote_granted(false);
```

#### 第二步：term 太旧，直接拒绝
如果：
```cpp
request.term() < current_term_
```
就直接返回。

#### 第三步：如果对方 term 更高，自己先退成 follower
如果：
```cpp
request.term() > current_term_
```
就调用：
```cpp
BecomeFollowerLocked(request.term(), -1, "received higher term vote request")
```

#### 第四步：比较 candidate 的日志是否足够新
调用：
```cpp
IsCandidateLogUpToDateLocked(request.last_log_index(), request.last_log_term())
```

比较规则是：
1. 先比最后一条日志的 term
2. term 相同再比最后一条日志的 index

这一步是 Raft 保证安全性的关键之一。

#### 第五步：检查当前 term 是否还能投票
```cpp
const bool can_vote = (voted_for_ == -1 || voted_for_ == request.candidate_id());
```

也就是：
- 还没投过票，可以投
- 已经投给同一个 candidate，也可以认为继续投它

#### 第六步：满足条件则投票
如果同时满足：
- `can_vote`
- `up_to_date`

那么：
- `voted_for_ = request.candidate_id()`
- `response->set_vote_granted(true)`
- `ResetElectionTimerLocked()`

为什么投票后要重置选举超时？
因为这表示当前节点已经认可这轮选举，不应该马上自己再发起一轮新的选举。

---

### 5.6 candidate 如何变成 leader

在 `StartElection()` 发出的每个 RPC 回包里，如果：
- `response->vote_granted() == true`

就把 `votes` 加 1。

当：
```cpp
total >= quorum
```
时，调用：
```cpp
OnElectionWon(term)
```

---

### 5.7 `OnElectionWon()`
这个函数会再次检查：
- 自己是不是还在运行
- 自己是不是 candidate
- 当前 term 是否还是刚刚赢下来的那个 term

检查通过后调用：
```cpp
BecomeLeaderLocked()
```
然后立即发送一次心跳：
```cpp
SendHeartbeats()
```

这样集群其他节点就能尽快知道 leader 出现了。

---

## 6. Leader 初始化流程

### 6.1 `BecomeLeaderLocked()`
leader 上位时主要做几件事：

1. `role_ = Role::kLeader`
2. `leader_id_ = config_.node_id`
3. 取消选举定时器
4. 初始化复制进度：
   - `next_index_[peer] = last_log_index + 1`
   - `match_index_[peer] = 0`
5. 启动心跳定时器：`ResetHeartbeatTimerLocked()`

---

### 6.2 为什么 `next_index_` 初始是 `last_log_index + 1`
因为对 leader 来说：
- follower 理论上“下一条应该接收的日志”
- 默认先假设 follower 和自己一样新

如果后面发现 follower 不匹配，再慢慢回退。

---

## 7. 心跳与日志复制流程

在你的实现里，**心跳和日志复制走的是同一个 RPC：`AppendEntries`**。

区别只是：
- 如果 `entries` 为空，就是纯心跳
- 如果 `entries` 不为空，就是带日志的复制请求

---

### 7.1 Leader 如何发送心跳

入口函数：`RaftNode::SendHeartbeats()`

整体步骤：

1. 加锁确认自己还是 leader
2. 取出：
   - `peers`
   - `term`
   - `leader_commit`
3. 对每个 peer 提交一个线程池任务
4. 每个任务构造一个 `AppendEntriesRequest`

在构造请求时，会用到这个 follower 的：
- `next_index_[peer.node_id]`

然后计算：
- `prev_log_index = next_index - 1`
- `prev_log_term = log_[prev_log_index].term`

接着把 `[next_index, log_.size())` 这段日志全部带上。

所以你当前代码不是“只发空心跳”，而是：
- 如果 follower 落后，顺便带上缺失日志
- 如果 follower 不落后，entries 可能为空，就退化成普通心跳

---

### 7.2 follower 如何处理 `AppendEntries`

入口函数：`RaftNode::OnAppendEntries()`

处理顺序非常关键。

#### 第一步：先默认失败
```cpp
response->set_success(false);
```

#### 第二步：term 太旧，直接拒绝
如果：
```cpp
request.term() < current_term_
```
则直接返回。

#### 第三步：如果对方 term 更高，或者自己不是合格 follower，则转成 follower
调用：
```cpp
BecomeFollowerLocked(request.term(), request.leader_id(), "received append entries")
```

如果 term 没问题而且自己本来就是 follower，则：
- `leader_id_ = request.leader_id()`
- `ResetElectionTimerLocked()`

这一步非常重要，因为：
- 收到 leader 的心跳，说明 leader 还活着
- follower 就不应该自己发起选举

---

### 7.3 一致性检查：`prev_log_index` / `prev_log_term`

这是日志复制的关键。

#### 情况 1：`request.prev_log_index() >= log_.size()`
说明 follower 本地日志太短，连前置日志都没有。
直接失败返回。

#### 情况 2：
```cpp
log_[request.prev_log_index()].term != request.prev_log_term()
```
说明 follower 本地在这个位置的 term 对不上。
也失败返回。

含义是：
- 只有当前缀日志完全匹配时，后续日志才能接上

---

### 7.4 follower 如何处理冲突日志

如果前缀一致，就从：
```cpp
append_at = request.prev_log_index() + 1
```
开始处理新日志。

代码逻辑是：
1. 一边看请求里的 `entries`
2. 一边看本地 `log_`
3. 如果同一位置 term 不同，说明这里开始冲突
4. 直接：
```cpp
log_.resize(append_at)
```
把冲突位置及后面的本地日志全部删掉
5. 再把 leader 发来的剩余日志 append 到本地

这个过程就是 Raft 里的：
- 冲突日志删除
- 正确日志覆盖

---

### 7.5 follower 如何更新提交位置

如果：
```cpp
request.leader_commit() > commit_index_
```
说明 leader 已经提交得更远了。

那 follower 会把自己的提交位置推进到：
```cpp
min(request.leader_commit(), LastLogIndexLocked())
```

为什么要取 `min`？
因为 follower 不能提交超过自己本地实际已有日志的位置。

如果 commit 前进了，就把：
```cpp
should_apply = true;
```

函数解锁后再调用：
```cpp
ApplyCommittedEntries()
```

也就是说：
- **收到日志** 不等于立即 apply
- **收到 leader_commit 并推进 commit_index_** 后，才会 apply

---

## 8. 客户端提案流程

入口函数：`RaftNode::Propose(const Command &command)`

这是客户端写请求进入 Raft 的主要入口。

---

### 8.1 第一步：先检查当前节点能不能接请求

#### 检查 1：节点是否还在运行
如果节点正在停止：
- 返回 `kNodeStopping`

#### 检查 2：当前节点是否是 leader
如果不是 leader：
- 返回 `kNotLeader`
- 同时带上 `leader_id_`

所以你的 follower 是不会直接接收客户端写请求的。

---

### 8.2 第二步：校验业务命令
调用：
```cpp
ValidateCommandUnlocked(command, &reason)
```

这里主要检查：
- `command.IsValid()`
- `Serialize()` 后是不是空
- 命令大小有没有超过上限（当前上限是 1MB）

通过后，再把命令序列化成日志字符串：
```cpp
command_data = command.Serialize();
```

---

### 8.3 第三步：leader 本地先追加日志
调用：
```cpp
log_index = AppendLocalLogUnlocked(command_data);
```

这个函数做了两件事：

1. 往 `log_` 里 push 一条新日志
   - `index = log_.size()`
   - `term = current_term_`
   - `command = command_data`

2. 更新 leader 自己的复制状态
```cpp
match_index_[config_.node_id] = new_index;
next_index_[config_.node_id] = new_index + 1;
```

这里的含义是：
- leader 自己当然已经拥有这条日志
- 所以自己的 `match_index_` 可以直接推进

---

### 8.4 第四步：复制到多数派

调用：
```cpp
ReplicateLogEntryToMajority(log_index)
```

这个函数当前实现是**同步串行**地向 peers 发 `AppendEntries`。

它的逻辑大致是：
1. 先记录当前：
   - `term`
   - `leader_commit`
   - `peers`
2. 对每个 peer 构造 `AppendEntriesRequest`
3. 从 `next_index_[peer]` 开始把还没复制的日志带上
4. 发 RPC
5. 收到响应后：
   - 如果对方 term 更高，leader 退化成 follower
   - 如果成功：
     - 更新 `match_index_[peer]`
     - 更新 `next_index_[peer]`
     - `success_count++`
   - 如果失败：
     - `next_index_[peer]--`
6. 一旦 `success_count >= majority`，就返回 true

注意，这里当前是“最小可运行版本”，所以：
- 失败时只做简单回退 `next_index_--`
- 没有做更完整的冲突优化信息返回

---

### 8.5 第五步：推进 `commit_index_`

复制到多数派之后，`Propose()` 会调用：
```cpp
AdvanceCommitIndexUnlocked()
```

**按当前上传代码，这个函数还是简化实现：**
```cpp
if (last_index > commit_index_) {
    commit_index_ = last_index;
}
```

也就是说，当前仓库版本里：
- 它还没有完整使用 `match_index_` 按标准 Raft 规则推进提交
- 它更像一个“当前阶段先跑通”的实现

所以如果后面你按更标准的方式改这里，README 对应也要一起更新。

---

### 8.6 第六步：应用到状态机

提交推进后，`Propose()` 会调用：
```cpp
ApplyCommittedEntries()
```

它会把：
- 从 `last_applied_ + 1`
- 到 `commit_index_`

之间的所有日志，按顺序应用到状态机。

最后如果：
```cpp
last_applied_ < log_index
```
就认为这次提案虽然提交了，但还没有真正 apply 完，返回失败。

否则返回：
- `status = kOk`
- `message = "command committed and applied"`

所以在你当前代码里，一次 `Propose()` 成功的定义不是“日志写进 leader 本地”，而是：
- 复制成功
- commit 推进成功
- apply 成功

---

## 9. 状态机应用流程

### 9.1 `ApplyCommittedEntries()`
这个函数是日志和业务状态之间的桥梁。

它的执行步骤是：

1. 先拿 `apply_mu_`
   - 保证一次只有一个线程在做 apply

2. 循环检查：
   - 如果 `last_applied_ >= commit_index_`
   - 说明没有新提交日志需要 apply
   - 直接返回

3. 取下一条待应用日志：
```cpp
apply_index = last_applied_ + 1;
command_data = log_[apply_index].command;
```

4. 解读状态机对象：
```cpp
state_machine = state_machine_.get();
```

5. 调用状态机：
```cpp
state_machine->Apply(apply_index, command_data)
```

6. 如果成功，把：
```cpp
last_applied_ = apply_index;
```

7. 然后继续下一条，直到追平 `commit_index_`

这保证了一个很重要的性质：
- **状态机只会按日志顺序执行**
- 不会跳着执行，也不会并发执行同一段日志

---

### 9.2 `KvStateMachine::Apply()`
当前状态机很直观：

1. `Command::Deserialize(command_data, &cmd)`
   - 把日志字符串恢复为业务命令

2. `cmd.IsValid()`
   - 再做一次校验

3. 加锁保护 `kv_`

4. 根据 `cmd.type` 执行：
   - `kSet`：
     ```cpp
     kv_[cmd.key] = cmd.value;
     ```
   - `kDelete`：
     ```cpp
     kv_.erase(cmd.key);
     ```

所以日志真正“落到业务状态”是在这里发生的。

---

## 10. `Describe()` 输出该怎么读

`RaftNode::Describe()` 会输出：
- `node`
- `role`
- `term`
- `voted_for`
- `leader`
- `last_log_index`
- `commit_index`
- `last_applied`
- `kv`

这是你当前调试最有价值的观察窗口。

可以这样理解：

### `last_log_index`
说明日志已经写到哪里了。

### `commit_index`
说明哪些日志已经“提交”。

### `last_applied`
说明哪些日志已经真正执行到状态机了。

### `kv`
说明业务结果是什么。

如果某个时刻：
- `last_log_index = 10`
- `commit_index = 8`
- `last_applied = 8`

说明：
- 节点本地已经存了 10 条日志
- 只有前 8 条确认提交
- 状态机也只执行到第 8 条
- 第 9 和第 10 条还不能对外当作已生效

---

## 11. `main.cpp` 的测试流程怎么理解

`main.cpp` 当前更像一个“集成联调 demo”，不是完整的故障测试框架。

它主要做了这些事：

### 11.1 启动 3 节点集群
调用 `BuildThreeNodeConfigs()` 生成：
- node-1: `127.0.0.1:50051`
- node-2: `127.0.0.1:50052`
- node-3: `127.0.0.1:50053`

每个节点都有：
- 选举超时：300~600ms
- 心跳间隔：100ms
- RPC 超时：500ms

---

### 11.2 等待 leader 出现
`WaitForLeader()` 会周期性检查 `Describe()` 字符串里是否包含 `role=Leader`。

一旦找到 leader，就开始后面的写入测试。

---

### 11.3 正常路径命令测试
`main.cpp` 会依次测试：
- `SET x 1`
- `SET y 2`
- `SET x 100`
- `DEL y`
- `DEL not_exist`
- `SET z 999`
- `DEL x`
- `SET x final`

每次都通过：
```cpp
ProposeAndWait(...)
```
去：
1. 调用 `leader->Propose(cmd)`
2. 打印 `ProposeResult`
3. sleep 一小段时间
4. 打印所有节点快照

所以你能直接在日志里看到：
- 哪个节点是 leader
- 写完后各节点 `last_log_index` 是否一致
- `commit_index` / `last_applied` 是否一致
- `kv` 是否一致

---

### 11.4 非法请求测试
#### 空 key 命令
`SET <empty> bad`

预期：
- leader 返回 `InvalidCommand`

#### 向 follower 直接提案
预期：
- follower 返回 `NotLeader`

这两个测试都能验证：
- 命令校验是否有效
- 非 leader 是否正确拒绝写请求

---

### 11.5 连续写入测试
最后会连续写入 `k0 ~ k9`。

这个测试主要验证：
- 在短时间连续提案时
- 日志复制、提交和应用链路是否还能保持一致

---

## 12. 变量在几个关键阶段的对照

这一节可以当成速查表。

---

### 12.1 follower 正常空闲时
- `role_ = kFollower`
- `leader_id_` 指向当前 leader
- `current_term_` 跟随集群最新 term
- `voted_for_` 可能是 `-1`，也可能是本 term 内投过票的 candidate
- `commit_index_` / `last_applied_` 随 leader 推进

---

### 12.2 发起选举时
- `role_`：Follower -> Candidate
- `current_term_++`
- `voted_for_ = 自己`
- `leader_id_ = -1`
- `votes = 1`

这一步最重要的变化就是：
- 进入新 term
- 先给自己投一票

---

### 12.3 当选 leader 时
- `role_ = kLeader`
- `leader_id_ = 自己`
- `next_index_[peer] = last_log_index + 1`
- `match_index_[peer] = 0`

这表示：
- 我现在负责对外提供写入服务
- 我准备开始维护每个 follower 的复制进度

---

### 12.4 leader 接收客户端写请求后
- `log_` 多一条新日志
- `match_index_[self] = new_index`
- `next_index_[self] = new_index + 1`

说明：
- leader 本地已经先写入这条日志

---

### 12.5 follower 复制落后时
如果某个 follower 回复 `AppendEntries` 失败：
- `next_index_[peer]--`

含义是：
- leader 认为这个 follower 的日志可能更旧
- 下次从更前面的位置开始尝试对齐

---

### 12.6 follower 收到 leader 已提交进度后
如果：
- `request.leader_commit() > commit_index_`

那么 follower 会：
- 推进自己的 `commit_index_`
- 再调用 `ApplyCommittedEntries()`
- 最后把 `last_applied_` 追上来

---

## 13. 当前实现的几个简化点

这一节非常重要，因为它决定了“这份代码现在到了哪个阶段”。

### 13.1 `AdvanceCommitIndexUnlocked()` 还是简化版本
当前上传代码里，它还没有完全按标准 Raft 的 `match_index_ + 当前 term` 规则推进提交。

所以目前它更像：
- 先跑通流程的版本

如果你后面把这里改成更标准的版本，README 也要同步更新这一节。

---

### 13.2 `ReplicateLogEntryToMajority()` 是同步串行复制
这让逻辑比较直观，但工程上还比较粗。

优点：
- 好理解
- 好调试

缺点：
- 性能一般
- 对复杂故障场景处理还不够细

---

### 13.3 失败回退是简单 `next_index_--`
当前没有做更完整的冲突优化。

这意味着：
- 能工作
- 但在冲突日志很多时效率不高

---

### 13.4 还没有持久化
当前这些状态都还在内存里：
- `current_term_`
- `voted_for_`
- `log_`

所以节点一重启，状态就丢了。

这说明当前代码还处于：
- **可运行的内存版 Raft**
而不是完整可恢复版本。

---

### 13.5 `main.cpp` 是 happy path 集成测试，不是完整故障测试
目前主要测到了：
- 能选主
- 能提案
- 能复制
- 能提交
- 能应用
- 三节点最终 KV 一致

还没系统测到：
- leader 宕机切换
- follower 掉队后追日志
- 网络分区
- 节点重启恢复
- 不同 term 下的复杂冲突

---

## 14. 建议你如何结合这份 README 看代码

推荐顺序：

### 第一遍：先抓主线
先看这些函数：
1. `Start()`
2. `OnElectionTimeout()`
3. `StartElection()`
4. `OnElectionWon()`
5. `BecomeLeaderLocked()`
6. `SendHeartbeats()`
7. `OnAppendEntries()`
8. `Propose()`
9. `ApplyCommittedEntries()`
10. `KvStateMachine::Apply()`

只理解“请求是怎么流动的”，先别纠结细节。

---

### 第二遍：再盯变量变化
重点盯：
- `role_`
- `current_term_`
- `voted_for_`
- `leader_id_`
- `log_`
- `commit_index_`
- `last_applied_`
- `next_index_`
- `match_index_`

每看一个函数，就问自己：
- 它改了哪些变量？
- 为什么必须改这些变量？
- 改完后节点语义发生了什么变化？

---

### 第三遍：结合 `main.cpp` 日志看
当你跑 `main.cpp` 时，重点盯输出中的：
- `role`
- `term`
- `leader`
- `last_log_index`
- `commit_index`
- `last_applied`
- `kv`

你会非常容易把代码和实际运行结果对应起来。

---

## 15. 一句话总结

这份项目当前已经实现了一个**最小可运行的 Raft 原型**：
- 能启动多节点
- 能选举 leader
- 能通过 `AppendEntries` 复制日志
- 能推进提交位置
- 能把已提交日志应用到 KV 状态机
- 能在 `main.cpp` 中观察三节点最终状态一致

但它仍然保留了一些“当前阶段的简化实现”，例如：
- commit 推进还可以更标准
- 复制失败回退还比较粗
- 没有持久化
- 测试还偏 happy path

所以最适合把它理解成：

> **一个已经跑通主流程、并且适合继续往工业化版本演进的 Raft 基础实现。**
