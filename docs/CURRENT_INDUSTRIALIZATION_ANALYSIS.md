# CURRENT_INDUSTRIALIZATION_ANALYSIS

## 1. 当前项目定位

### 当前项目现在像什么

当前项目更像一个“已经具备较完整 Raft 主流程验证能力的工程化原型”，而不是一个可直接作为线上一致性底座交付的分布式存储内核。

它已经不是最初级的课程 demo，原因是它具备以下特征：

- 有明确的模块边界：`node`、`replication`、`storage`、`state_machine`、`service`、`runtime`
- 有真实的持久化布局：`meta.bin`、segment log、snapshot 目录
- 有快照、追赶、重启恢复相关实现
- 有覆盖较广的 GoogleTest 测试集
- 有基础状态观测能力：`Describe()`、status/metrics RPC、结构化日志

但它也还不是工业级 Raft consistency substrate，原因是：

- 核心能力与 demo 层仍存在明显耦合
- transport、membership、故障注入、fsync 语义、诊断体系都还偏“验证型工程”
- 许多能力虽然存在，但更接近“可在 happy path 和少量 failure path 下工作”，而不是“对长期线上故障模式有清晰边界”

### 已有基础实现的能力

- leader election
- term / vote 持久化
- log append、match、majority commit、apply
- conflict hint 和回退
- lagging follower catch-up
- snapshot save / load / install
- snapshot 后日志压缩
- restart recovery
- identity file 校验
- status / health / metrics RPC
- segment log 和 snapshot 的基础校验

### 仍停留在 demo / happy path 的能力

- `raft_metadata_client` / `MetadataService` 是当前主要外部接口
- cluster membership 仍是静态配置
- transport 仍强依赖 gRPC，同进程内没有抽象 transport boundary
- durability 语义更接近“ofstream flush + rename 成功路径”，不是严格 power-loss 级别 durability
- snapshot atomicity 仍偏“可恢复式”，不是强事务式
- 可观测性更偏调试与人工排查，缺少生产事故诊断体系
- 测试广度不错，但 determinism、fault injection、crash simulation、磁盘错误和网络行为建模还不够

### KV 当前在项目中的实际作用

KV 在当前项目中不是最终产品雏形，而是：

- 提供 `Propose -> replicate -> commit -> apply` 的验证载体
- 提供一个最简单的 sample state machine
- 为 snapshot / restart recovery 提供可观察业务状态
- 为集群行为测试提供 demo 外部接口

换句话说，KV 现在是“Raft 内核验证层”，不是“未来系统核心产品层”。

### 哪些模块是核心 Raft 能力

- `modules/raft/node`
- `modules/raft/replication`
- `modules/raft/storage`
- `modules/raft/state_machine`
- `proto/raft.proto` 中的 `RaftService`
- `modules/raft/runtime`

### 哪些模块是 app / demo / service 层

- `apps/main.cpp`
- `apps/raft_metadata_client.cpp`
- `modules/raft/service`
- `proto/raft.proto` 中的 `KvService`
- `KvStateMachine` 的 KV 语义部分

---

## 2. 当前工业化成熟度评分

### 评分口径

这里的评分标准不是“像不像一个 KV 数据库产品”，而是“离可复用的 Raft / distributed storage consistency layer 还有多远”。

评分含义：

- `0-20`：概念验证
- `21-40`：demo / prototype
- `41-60`：工程化原型
- `61-80`：可控试运行级
- `81-100`：成熟工业级

### 总体评分

**总分：58 / 100**

依据：

- 强于纯 demo：已有持久化、快照、恢复、较完整测试集、模块化结构
- 弱于工业级 substrate：缺少 durability 明确语义、故障注入体系、membership 演进路径、transport 抽象、诊断与运维边界
- 当前最强的是“主流程正确性验证能力”
- 当前最弱的是“长期线上复杂故障和可运维性”

### 分模块评分

| 模块 | 评分 | 依据 |
| --- | ---: | --- |
| `node` | 62 | 功能完整度高，覆盖 election/commit/apply/recovery/snapshot orchestration，但职责过重，后续改动风险高 |
| `replication` | 63 | 已有批量复制、conflict hint、snapshot 切换、backoff；但只验证了部分 failure mode，缺少更严格 transport/fault 模型 |
| `storage` | 54 | 有 segmented log、checksum、legacy load、tail truncate、snapshot catalog，但 durability 语义仍偏弱，缺少 fsync/故障注入/磁盘错误处理分层 |
| `state_machine abstraction` | 48 | 已有 `IStateMachine`，但 `RaftNode` 仍有 KV 约束、KV limits、`DebugGetValue` 等耦合，抽象不够“substrate-first” |
| `service` | 46 | Raft RPC 与 KV RPC 均可用，但仍在同一 proto 中、同一服务适配层内共存，边界不够工业化 |
| `runtime` | 57 | timer/thread-pool/logging 足以支撑当前工程，但缺少更强的 lifecycle diagnostics、race hardening 和 executor 抽象 |
| `apps` | 35 | 适合 demo 和 acceptance，明显不是长期产品入口层 |
| `proto / contract` | 45 | Raft contract 基本齐全，但和 KV contract 混放，同一 package 内承载内部/外部两层语义 |
| `tests` | 66 | 功能覆盖面相当不错，尤其 snapshot/restart/catch-up；但 deterministic、fault injection、disk/network failure 建模不足 |
| `工程化支撑` | 44 | 有 CMake、preset、分组测试脚本、AGENTS 索引；但看不到 CI、sanitizer、clang-format、clang-tidy、发布/兼容策略 |

---

## 3. 当前问题分类

### 3.1 Raft 正确性

#### leader election

现状：

- 有独立选举超时、candidate -> leader 迁移、单节点自选主路径
- 有 election 测试、split-brain 场景测试

评价：

- 对当前静态集群和低复杂度网络模型来说，基础能力较完整
- 但没有 deterministic timing 框架，也没有随机化长期压力验证

#### term / vote

现状：

- `current_term`、`voted_for` 持久化
- higher term reply 会触发 follower 回退
- vote 授予依赖 log up-to-date 检查

评价：

- 主路径合理
- 风险主要在未来修改时，`RaftNode` 中状态与持久化时机耦合较深，不容易局部演进

#### log matching

现状：

- 有 `prev_log_index` / `prev_log_term` 校验
- 有 conflict hint
- 支持 follower 冲突时回退 `next_index`

评价：

- 已超出最简单逐项回退 demo
- 但正确性更多依赖实现逻辑和测试覆盖，而不是明确的不变量文档和更强的仿真验证

#### majority commit

现状：

- leader 基于 `match_index` 推进 commit
- 只提交当前任期多数复制的日志

评价：

- 核心语义方向正确
- 缺少更系统的 invariant 文档和故障边界证明

#### leader completeness

现状：

- 通过 candidate log up-to-date 检查和 no-op entry 路径间接支撑

评价：

- 实现上具备基础，但没有明确以“leader completeness”命名和覆盖的规范层文档

#### split-brain 防护

现状：

- 有分裂脑相关测试
- higher term message 会迫使 leader step down

评价：

- 对当前测试覆盖场景有效
- 对真实网络抖动、延迟重排、长尾不可达组合场景没有工业级信心

#### follower catch-up

现状：

- 有 batch append
- 有 conflict hint
- 日志被压缩时会切到 snapshot install

评价：

- 是当前项目较强的一块
- 仍缺少更长时间 churn、反复落后/恢复场景的强验证

#### snapshot install

现状：

- 有 `InstallSnapshot`
- follower 安装 snapshot 后恢复状态机并压缩日志

评价：

- 主能力存在
- 但 snapshot 写入 atomicity 和安装中断后恢复语义仍偏工程原型

#### restart recovery

现状：

- 支持 persistent state load
- 支持 latest valid snapshot load
- 支持 committed log replay

评价：

- 这是当前项目的亮点之一
- 但 crash point 仍以有限测试样例为主，没有全面 crash matrix

### 3.2 Replicated log

#### log append

- 已支持本地 append、复制、多数提交、apply
- 仍然绑定 `Command` 的字符串化表示，离通用 replicated log substrate 还有距离

#### log truncate

- follower 冲突时能截断尾部
- snapshot 后能压缩前缀
- 没有“通用日志条目类型系统”，只有 command string 和 snapshot marker

#### log index / term 查询

- `FirstLogIndexLocked`、`LastLogIndexLocked`、`TermAtIndexLocked` 等能力齐全
- 这些 API 仍深嵌在 `RaftNode` 内，而不是独立 log abstraction

#### commit index

- 已实现
- 但 commit 生命周期仍与 `RaftNode` 全局状态 tightly coupled

#### apply index

- 已实现并持久化
- 对状态机恢复流程有考虑

#### log compaction

- 已支持 snapshot 后前缀压缩
- 但 compaction policy 仍很简单，且 marker 语义耦合在 node core 中

#### log consistency check

- 有 prev-log matching、conflict hint、segment checksum
- 但缺少一层更明确的 replicated-log API 和 invariant 说明

### 3.3 持久化可靠性

#### hard state

- 有 `meta.bin`
- term/vote/commit/applied 被保存
- 问题在于 durability 级别没有明确升到 fsync 语义

#### log entry

- 有 segment 文件
- 有 entry header 和 checksum
- 对 torn tail 有 truncate 处理

#### segment append / truncate

- 当前更接近“重写完整 temp log dir 再替换”而不是高性能 WAL append-only 设计
- 对实验项目有利于简单和可恢复，但离工业级 log engine 仍远

#### fsync 策略

- 目前没有看到显式 fsync 策略
- 主要依赖 `flush()` 和文件/目录替换
- 这是 durability 工业化差距中最明显的一项

#### partial write

- segment reader 对损坏尾部有 truncate
- snapshot 有 checksum fallback
- 但 power-loss 下跨文件、跨目录、rename 顺序相关问题仍无强保证

#### checksum

- segment log 和 snapshot 数据都有 checksum/校验能力
- `meta.bin` 本身没有看到额外校验

#### crash recovery

- 有基础能力
- 缺少 crash point 枚举验证，如：
  - meta 已写、log 未落稳
  - log 替换中断
  - snapshot data 与 meta 之间中断

#### snapshot atomicity

- 当前实现明确写“不会创建 temp snapshot directory”
- 这对调试友好，但不属于强 atomic snapshot publish 设计

### 3.4 Snapshot / recovery

#### snapshot save/load

- 已支持
- 可回退到较旧的 valid snapshot

#### install snapshot

- 已支持
- 仍缺少更强的 interrupted install and restart-after-interruption 行为建模

#### restore state machine

- 已支持
- 当前只对 KV snapshot format 生效

#### log compaction after snapshot

- 已支持
- 风险点在 node core 中状态一起推进，复杂度高

#### lagging follower catch-up

- 这是当前项目强项
- 但还没有脱离 demo 状态进入“可独立复用的 replicated log catch-up subsystem”

### 3.5 State machine / KV 耦合

#### 当前 KV 是否与核心 Raft 耦合

有明显耦合。

表现：

- `NodeConfig` 内含 `KvRequestLimits`
- `RaftNode` 暴露 `ValidateKey`、`ValidateValue`
- `RaftNode` 暴露 `DebugGetValue`
- `KvServiceImpl` 直接依赖 `CommandType::kSet / kDelete`
- `RaftNode::Describe()` 会尝试 dynamic_cast 到 `KvStateMachine`

#### 当前 state machine 抽象是否足够

`IStateMachine` 是一个合格起点，但还不够工业化。

优点：

- 已有 `Apply`
- 已有 `SaveSnapshot`
- 已有 `LoadSnapshot`

不足：

- 仍没有将“用户命令”“系统日志项”“状态机批处理/幂等语义”分层
- 没有 snapshot provider / restore boundary 的更细粒度角色划分
- 缺少对 future storage engine 的接口约束文档

#### KV 哪些部分只是 demo

- `Command` 的 `SET|...` / `DEL|...`
- `KvService`
- `KvStateMachine`
- KV request limits
- `DebugGetValue()` 和围绕 KV 的 status 验证逻辑

#### 哪些地方会阻碍未来接入真实 storage engine

- `Command` 仍被假设为字符串命令
- `RaftNode` 仍掌握 KV 级输入校验
- snapshot restore 接口过于贴近当前单文件 KV snapshot
- `KvService` 与 Raft service 同处一个 proto

### 3.6 RPC / transport 边界

#### Raft internal RPC 和 KV/client RPC 是否边界清晰

不够清晰。

现状：

- `RaftService` 和 `KvService` 在同一个 `proto/raft.proto`
- 两者都依赖同一个 node runtime
- `RaftNode` 同时对内承载 Raft RPC、对外承载 KV demo 行为

结论：

- 当前边界“能用”，但不利于转型为纯一致性层

#### timeout / retry / error code / unavailable node handling 是否足够

- Raft internal RPC 有 deadline
- per-peer replication 有 backoff
- KV RPC 有状态码和 leader redirect 语义

不足：

- 没有统一 transport error taxonomy
- 没有 transport 抽象层
- unavailable node 的行为主要通过 gRPC status + timeout 间接处理

#### transport 是否容易替换

不容易。

原因：

- `RaftNode` 直接持有 gRPC channel/stub
- `PeerClient`、RPC 方法、service impl 都直接绑定 gRPC 类型

### 3.7 Membership / configuration

#### single-node config

- 支持
- 可自举为 leader

#### static multi-node config

- 支持
- 通过 `node.<id>=host:port` 静态配置

#### peer list

- 支持静态 peer list
- 不支持动态变更

#### local node identity

- 有 `node_identity.txt`
- 对误用旧 data dir 有保护作用

#### config validation

- 对 node id、成员存在性、基本布尔/整数参数有基础校验

#### future membership change 的缺口

- 缺少联合共识或任何 membership change 设计
- 当前配置模型也不是为 membership evolution 设计的

### 3.8 并发和生命周期

#### thread pool

- 足够支撑当前规模
- 不是可控 executor framework

#### timer

- 有 generation 防 stale callback
- 属于比较务实的工程实现

#### shutdown

- 有 `Stop()`
- 有 server shutdown、scheduler stop、snapshot worker stop

不足：

- 没有系统化 shutdown 阶段模型
- 没有针对关闭时 RPC / callback / snapshot interplay 的专项验证框架

#### callback lifetime

- 大量使用 `weak_from_this()`
- 对典型生命周期悬挂已有防御

#### lock order

- 存在 `mu_`、`apply_mu_`、`snapshot_mu_`、`metrics_mu_`
- 当前代码能工作，但锁层次较复杂，后续演进风险高

#### data race risk

- 基础互斥做得不错
- 但由于 `RaftNode` 职责集中，边界多，race 风险更多来自未来修改和复杂故障

#### node start/stop/restart lifecycle

- 已具备功能
- 但“工业级生命周期 contract”仍未显式化

### 3.9 可观测性

#### logs

- 有结构化 key-value 风格 stdout 日志
- 能用于人工排查

不足：

- 无日志分流
- 无 trace id / request id / peer op id
- 无日志等级动态变更机制之外的采样/压缩策略

#### metrics

- 有 propose/election/snapshot/storage/RPC 指标快照
- 基础有效

不足：

- 不是 Prometheus/OpenTelemetry 风格
- 无持久化历史，无报警边界

#### tracing

- 没有 tracing

#### raft state dump

- `Describe()` 和 `Status()` 提供基础 dump
- 对当前阶段很实用

#### replication status

- status RPC 暴露 `match_index/next_index`
- 这是正向资产

#### snapshot status

- 只有有限暴露
- 没有完整 snapshot lifecycle 诊断模型

#### recovery diagnostics

- 有部分日志和状态
- 缺少“为什么回放了哪些日志、为什么选中哪个 snapshot”的标准诊断输出

### 3.10 测试体系

#### unit tests

- 有

#### integration tests

- 有，覆盖 cluster behavior

#### persistence tests

- 有

#### restart tests

- 有，且覆盖面相对不错

#### failure injection

- 几乎没有体系化 failure injection

#### crash simulation

- 以场景测试为主，没有 crash matrix 和精细 crash point 注入

#### network partition

- 有 split-brain 相关测试
- 但不是通用网络仿真框架

#### benchmark / smoke tests

- 有 acceptance shell script
- 没有系统 benchmark

#### deterministic tests

- 缺少 deterministic scheduler / virtual time / simulated transport

### 3.11 工程化

#### CMake

- 结构清晰
- target 划分合理

#### target structure

- `raft_proto` / `metadata_proto` / `raft_core` / `raft_demo` / `raft_metadata_client`
- 对当前阶段够用

#### include structure

- 已按模块整理
- 对后续迭代有帮助

#### CI

- 仓库中未看到明显 CI 配置

#### sanitizer

- 未看到 sanitizer 配置

#### clang-format

- 未看到 `.clang-format`

#### clang-tidy

- 未看到 `.clang-tidy`

#### README

- 按仓库规则本次未读取 `README.md`
- 因此不能评价其质量，只能说当前工程入口文档主要依赖 `AGENTS.md`、`test.sh`、`scripts/acceptance_cluster.sh`

#### scripts

- 有测试分组脚本和 acceptance script
- 这是工程正向资产

#### AGENTS.md 索引是否足够支持后续迭代

- 当前索引足以支持后续模块化阅读
- 对分析/设计/小步变更很有帮助

---

## 4. Top 10 当前风险

| 风险 | 影响 | 涉及模块 | 优先级 | 可能导致 Raft safety 问题 | 可能导致数据丢失 | 可能导致线上不可诊断 |
| --- | --- | --- | --- | --- | --- | --- |
| 缺少明确 fsync/durability 策略 | power loss 后持久化边界不可信 | `storage`, `node` | P0 | 否 | 是 | 否 |
| snapshot publish 非强原子 | 崩溃点下可能出现 snapshot 与元数据不一致 | `storage`, `node`, `state_machine` | P0 | 否 | 是 | 部分是 |
| `RaftNode` 职责过重 | 后续修改容易误伤 correctness invariant | `node`, `replication`, `storage` | P0 | 是 | 是 | 是 |
| KV 语义渗入核心层 | 阻碍 consistency layer 抽象，未来演进风险高 | `common`, `node`, `service`, `state_machine` | P1 | 否 | 否 | 否 |
| transport 直接绑定 gRPC | 难以替换、难做精细故障建模 | `service`, `node`, `apps`, `proto` | P1 | 间接是 | 否 | 是 |
| 缺少 deterministic / fault-injection 测试 | safety 与 recovery 边界未被充分证明 | `tests`, 全模块 | P1 | 是 | 是 | 是 |
| membership 仅静态配置 | 无法向真实存储集群管理演进 | `common`, `apps`, `node` | P1 | 否 | 否 | 否 |
| 锁与生命周期复杂度高 | shutdown / callback / snapshot 并发边界难维护 | `node`, `runtime`, `replication` | P1 | 是 | 可能 | 是 |
| contract 层混合 Raft 与 KV | 产品边界不清，不利于一致性层化 | `proto`, `service`, `apps` | P2 | 否 | 否 | 否 |
| 可观测性缺 tracing 与恢复诊断 | 线上故障定位成本高 | `runtime`, `node`, `service`, `apps` | P2 | 否 | 否 | 是 |

---

## 5. 当前最可能导致数据丢失的问题

**最可能的问题是持久化路径缺少明确的 fsync / directory fsync / publish ordering durability 语义。**

原因：

- 当前 `storage` 和 `snapshot` 更多依赖 `flush()`、临时文件/目录替换、文件系统行为
- 这对“进程正常退出”或“轻度损坏恢复”是有帮助的
- 但对 power loss、内核崩溃、文件系统缓存未落盘场景，不足以给出工业级 durability 保证

具体高风险点：

- `meta.bin` 与 log dir 替换之间的崩溃窗口
- snapshot `data.bin` 与 `__raft_snapshot_meta` 发布窗口
- 缺少目录级持久化屏障

---

## 6. 当前最可能导致 Raft safety 破坏的问题

**最可能的 safety 风险不在已覆盖的基础 happy path，而在“snapshot/log compaction/restart/replication 交织路径”的边界条件。**

原因：

- 这部分逻辑横跨 `node`、`replication`、`storage`、`state_machine`
- 状态变量多：`last_snapshot_index`、`last_snapshot_term`、`commit_index`、`last_applied`、`next_index`、`match_index`
- 当前测试已覆盖若干关键场景，但还没有 deterministic fault model 来系统证明边界

这不是说当前已经存在明确 safety bug，而是说：

- **最可能隐藏 safety 问题的区域**就在这里
- 尤其当后续继续迭代或引入更复杂存储状态机时，风险会快速放大

---

## 7. 当前最可能导致线上不可诊断的问题

**最可能导致线上不可诊断的问题，是缺少系统化恢复诊断和跨请求/跨 peer 的 tracing 语义。**

现状虽然有：

- 结构化日志
- status/metrics
- `Describe()`

但仍缺：

- 某次复制链路的统一 request id / replication op id
- snapshot 选择与恢复决策的标准化事件
- restart replay 的详细诊断
- 持久化失败后的分类与可聚合错误码

结果是：

- 能看出“出错了”
- 不容易快速回答“为什么出错、出错前后状态是否一致、哪一侧先偏离”

---

## 8. 当前测试体系缺口

最主要缺口有：

1. 缺少 deterministic test harness
2. 缺少网络层 fault injection
3. 缺少磁盘错误、磁盘满、权限错误、rename 异常等注入
4. 缺少 crash point matrix
5. 缺少高 churn 长时间测试
6. 缺少状态机抽象替换测试
7. 缺少 membership 演进预备测试
8. 缺少性能 smoke 和回归基线
9. 缺少 sanitizer/TSan/ASan 驱动测试入口
10. 缺少契约兼容性和持久化兼容性回归检查

补充说明：

- 当前测试**不是少**，而是“故障模型类型仍不够工业级”
- 现在的测试体系已经足以支持“基线稳定化 + 小步演进”
- 但还不足以支撑“把它当成通用一致性底座长期演化”

---

## 9. 当前不应该立刻动的地方

### 不应立刻重写 `RaftNode`

原因：

- 这是当前系统的真实控制中枢
- 逻辑虽然集中，但已被现有测试体系覆盖到较多场景
- 直接拆大类很容易引入 safety 回归

### 不应立刻把 KV 全删掉

原因：

- KV 现在是最直接的 smoke/integration verification surface
- 删除过早会削弱现有验证闭环
- 正确做法是先降级为 sample/demo state machine，再逐步抽离

### 不应立刻改持久化格式

原因：

- 当前已有 segment/meta/snapshot 测试基线
- 在没有更完整 crash/durability spec 之前改格式，只会扩大风险面

### 不应立刻重做 transport

原因：

- gRPC 虽然绑定较深，但当前至少可用
- 在 correctness baseline、persistence baseline 未进一步固化前替换 transport 会同时引入太多变量

### 不应立刻把 snapshot 路径“优化”为复杂异步流水线

原因：

- snapshot 已经跨越 node/state_machine/storage 多模块
- 在没有更清晰的 publish/atomicity spec 前做性能优化，会先扩大 correctness 风险

### 不应立刻引入 dynamic membership 实现

原因：

- 当前连 static membership 的工业化边界都还未完全固化
- membership change 是高风险特性，应该放在 core correctness、persistence、snapshot、transport 边界更稳定之后

---

## 结论

当前项目最适合被定义为：

**“一个具备较完整 Raft 主流程、持久化、snapshot/recovery、测试覆盖的工程化原型；它已经明显超出课堂 demo，但距离工业级分布式存储一致性层还有一段系统化补强路径。”**

它最值得保留的资产是：

- 已跑通的 Raft 主链路
- 已存在的持久化和恢复结构
- 已较完整的测试集合
- 已经形成的模块化目录与 AGENTS 索引

它最需要警惕的短板是：

- durability 语义不够工业化
- KV 仍渗入核心边界
- transport / membership / observability / fault model 还没有 substrate 级抽象
