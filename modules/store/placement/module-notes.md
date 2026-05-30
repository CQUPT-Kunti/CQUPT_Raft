# store/placement 说明

## 模块职责

`modules/store/placement` 负责 StorageNode 副本候选节点的本地选择策略。

当前只负责：

- `StorageNodePlacementCandidate` 候选节点模型
- `ReplicaPolicy` 副本数量和最小成功数约束
- `PlacementRequest` 到 `PlacementDecisionResult` 的纯策略计算
- 节点筛除原因记录、确定性排序和最小 zone spread 语义

当前不负责：

- 真实 heartbeat / node registry / 容量上报
- 上传协调器
- `StorageNodeClient::WriteChunk`
- metadata `CommitObject`
- Repair / Rebalance / GC
- Raft / metadata / RPC 接入

## 枚举与辅助函数

### `StorageNodeHealth`

- `kHealthy`
  - 节点当前可参与新写入。
- `kDegraded`
  - 节点有异常或降级，当前策略不再分配新副本。
- `kReadOnly`
  - 节点只允许读，不接受新写入。
- `kUnavailable`
  - 节点不可达或不可用。
- `kDraining`
  - 节点处于摘流或迁移阶段，当前策略不再给它新写入。

### `StorageNodeDiskPressure`

- `kLow`
  - 磁盘压力低，可正常参与写入。
- `kMedium`
  - 磁盘有一定压力，但当前策略仍允许写入。
- `kHigh`
  - 磁盘压力高，当前策略直接排除。
- `kFull`
  - 磁盘接近或已经写满，当前策略直接排除。

### `ToString(StorageNodeHealth)`

- 用于把健康状态转成稳定字符串，主要服务于排除原因和测试断言。

### `ToString(StorageNodeDiskPressure)`

- 用于把磁盘压力状态转成稳定字符串，主要服务于排除原因和测试断言。

## 主要结构

### `StorageNodeLoadSnapshot`

- 作用：
  - 表达某个候选节点在 placement 决策时刻看到的负载快照。
- 字段：
  - `active_reads`
    - 当前活跃读请求数。
  - `active_writes`
    - 当前活跃写请求数。
  - `queued_ops`
    - 当前排队但尚未执行的操作数。
- 成员函数：
  - `TotalInflight()`
    - 返回 `active_reads + active_writes + queued_ops`。
    - 这是排序时使用的第一层负载指标，值越小越优先。

### `StorageNodePlacementCandidate`

- 作用：
  - 描述一个可供选择的 StorageNode 候选节点，以及 placement 决策需要的最小属性。
- 字段：
  - `node_id`
    - 节点逻辑标识。为空时该候选会被视为非法并排除。
  - `endpoint`
    - 节点网络地址或 RPC endpoint。当前策略不按它排序，但会随选择结果一起返回给调用方。
  - `health`
    - 节点健康状态。当前只有 `kHealthy` 允许参与新写入。
  - `disk_pressure`
    - 节点磁盘压力等级。`kHigh` 和 `kFull` 会被排除。
  - `total_capacity_bytes`
    - 节点总容量。当前主要作为观测字段保留，不直接参与排序判断。
  - `used_capacity_bytes`
    - 节点已用容量。当前主要作为观测字段保留，不直接参与排序判断。
  - `available_capacity_bytes`
    - 当前可用容量。先用于容量过滤，再作为首要排序条件。
  - `load`
    - 节点当前的读写负载快照。
  - `write_admission_overloaded`
    - 写入接纳层是否已明确过载。为 `true` 时直接排除。
  - `zone`
    - 故障域或可用区。`prefer_distinct_zones=true` 时优先用于跨 zone 选点。
  - `rack`
    - 机架字段，占位给后续更细粒度 spread 策略；当前未参与决策。
- 成员函数：
  - `CanFit(required_bytes, reserve_bytes)`
    - 判断当前节点在写入 `required_bytes` 后，是否还能保留至少 `reserve_bytes` 的剩余容量。
    - 当前用于排除容量不足节点。
  - `HasWritableHealth()`
    - 判断该节点是否可接收新写入。
    - 当前实现只把 `kHealthy` 视为可写。

### `ReplicaPolicy`

- 作用：
  - 描述一次副本选择的最小策略约束。
- 字段：
  - `replica_count`
    - 希望选出的副本节点数量。默认值为 `3`。
  - `minimum_successful_writes`
    - 后续真正执行写入时，至少需要多少个副本 durable success 才能视为满足提交前提。默认值为 `2`。
  - `avoid_same_node`
    - 是否禁止重复选择同一 `node_id`。当前实现按这个默认语义工作，不会输出重复节点。
  - `prefer_distinct_zones`
    - 是否优先跨 zone 选点。为 `true` 时先尽量选不同 zone，再按常规排序补齐。
  - `reserve_capacity_bytes`
    - 每个候选节点在放下当前 chunk 之后还必须额外保留的剩余容量。

### `PlacementRequest`

- 作用：
  - 封装一次副本选择所需的输入。
- 字段：
  - `identity`
    - chunk 身份信息。可以直接给 `chunk_id`，也可以给 `object_id + version + chunk_index` 让策略层生成 `chunk_id`。
  - `chunk_size_bytes`
    - 本次要放置的 chunk 大小。必须大于 `0`。
  - `policy`
    - 本次选择使用的副本策略。
  - `excluded_nodes`
    - 调用方显式要求跳过的节点 ID 列表。
  - `decision_epoch`
    - 决策版本或快照纪元，由上层传入并原样带到输出，便于调用方关联自己的候选集版本。

### `PlacementNodeExclusion`

- 作用：
  - 记录某个候选节点为什么没被选上。
- 字段：
  - `node_id`
    - 被排除的节点 ID。
  - `reason`
    - 排除原因，例如健康不可写、容量不足、显式排除、磁盘压力高、写入过载等。

### `PlacementDecision`

- 作用：
  - 描述一次成功或失败选择所携带的决策细节。
- 字段：
  - `chunk_id`
    - 最终确认的 chunk ID。来自 `identity.chunk_id`，或由 `object_id/version/chunk_index` 推导生成。
  - `replica_nodes`
    - 最终选中的副本节点列表，顺序即策略输出顺序。
  - `required_replica_count`
    - 本次目标副本数，来自 `policy.replica_count`。
  - `minimum_successful_writes`
    - 本次最小成功副本数，来自 `policy.minimum_successful_writes`。
  - `excluded_nodes`
    - 被筛掉的候选节点及其原因。
  - `decision_epoch`
    - 从请求透传的决策纪元。
  - `reasons`
    - 面向上层的补充决策说明。当前主要用于记录 zone spread 等策略说明，便于后续扩展。

### `PlacementDecisionResult`

- 作用：
  - 统一封装 placement 调用结果。
- 字段：
  - `status`
    - 结果状态码。成功为 `kOk`，参数问题返回 `kInvalidArgument`，候选不足返回 `kNodeUnavailable`。
  - `error_detail`
    - 失败时的人类可读错误说明。
  - `decision`
    - 决策主体。即使失败，也会尽量带上已解析出的 `chunk_id`、约束参数和被排除节点原因，便于调试。
- 成员函数：
  - `ok()`
    - 判断 `status == kOk`。

## 核心函数

### `ReplicaPolicySelector::SelectReplicas(const PlacementRequest&, std::span<const StorageNodePlacementCandidate>)`

- 作用：
  - 对给定候选节点执行筛选、排序和副本挑选，产出 placement 决策。
- 主要输入约束：
  - `chunk_size_bytes > 0`
  - `replica_count > 0`
  - `minimum_successful_writes > 0`
  - `minimum_successful_writes <= replica_count`
  - 请求必须携带可解析的 chunk identity
  - 候选列表不能为空
- 主要处理流程：
  - 解析或生成 `chunk_id`
  - 逐个候选执行可写健康、磁盘压力、容量、显式排除、过载等资格检查
  - 把被排除节点写入 `decision.excluded_nodes`
  - 对可用候选执行稳定排序
  - 如开启 `prefer_distinct_zones`，先尽量跨 zone 选点
  - 选满 `replica_count`，或在候选不足时返回明确错误
- 返回语义：
  - 成功时返回 `kOk`，并在 `decision.replica_nodes` 中给出最终节点列表
  - 请求非法时返回 `kInvalidArgument`
  - 合法候选不足以满足 `replica_count` 时返回 `kNodeUnavailable`

## `replica_policy.cpp` 实现细节

### 匿名命名空间内部结构和辅助函数

### `RankedCandidate`

- 作用：
  - 这是 `replica_policy.cpp` 内部使用的排序包装结构，不对外暴露。
  - 它把原始 `StorageNodePlacementCandidate` 和 `original_index` 绑在一起，保证最终排序在完全相同条件下仍可稳定落回原始输入顺序。
- 字段：
  - `candidate`
    - 原始候选节点副本。
  - `original_index`
    - 该候选在输入数组中的位置，用于排序最后一层稳定兜底。

### `AddExclusion(PlacementDecision*, std::string_view, std::string)`

- 作用：
  - 向 `decision.excluded_nodes` 追加一条排除记录。
- 细节：
  - 如果 `decision == nullptr`，函数直接返回，不会崩溃。
  - 这个 helper 被资格检查和重复节点跳过逻辑复用。

### `ResolveChunkId(const ChunkIdentity&, ChunkId*, std::string*)`

- 作用：
  - 把 `PlacementRequest.identity` 解析成最终参与决策的 `chunk_id`。
- 处理顺序：
  - 如果 `out_chunk_id == nullptr`，返回 `kInvalidArgument`
  - 如果请求已经给了 `identity.chunk_id`，直接校验这个值
  - 如果没有直接给 `chunk_id`，则要求 `object_id` 非空且 `version != 0`
  - 满足条件后调用 `MakeChunkId(...)` 生成 `chunk_id`
- 返回语义：
  - 成功返回 `kOk`
  - identity 信息不完整或 `chunk_id` 非法时返回 `kInvalidArgument`

### `EvaluateCandidateEligibility(const StorageNodePlacementCandidate&, const PlacementRequest&, const std::unordered_set<std::string>&)`

- 作用：
  - 对单个候选节点执行“是否有资格参与排序和选点”的前置检查。
- 返回语义：
  - 返回 `std::nullopt` 表示节点合格
  - 返回字符串表示该节点被排除的原因
- 当前检查顺序：
  - `node_id` 不能为空
  - 节点不能在 `excluded_nodes` 显式排除集合中
  - `HasWritableHealth()` 必须为真
  - `disk_pressure` 不能是 `kHigh` 或 `kFull`
  - `write_admission_overloaded` 不能为真
  - `CanFit(chunk_size_bytes, reserve_capacity_bytes)` 必须为真

### `PreferDistinctZone(const StorageNodePlacementCandidate&, const std::unordered_set<std::string>&)`

- 作用：
  - 判断某个候选节点是否能在“优先跨 zone”阶段贡献一个新的 zone。
- 返回为真的条件：
  - `candidate.zone` 非空
  - 该 zone 目前还没有被已选节点占用

## `SelectReplicas()` 的实际执行流程

### 1. 初始化输出骨架

- 在真正做选择前，先把以下信息写入 `result.decision`：
  - `required_replica_count`
  - `minimum_successful_writes`
  - `decision_epoch`
- 这样即使后面提前失败，调用方也能拿到基础约束和上下文。

### 2. 解析 chunk identity

- 调用 `ResolveChunkId(...)` 生成或校验 `decision.chunk_id`
- 如果 identity 无法解析，整个选择直接失败

### 3. 校验请求本身是否合法

- `chunk_size_bytes == 0` 返回 `kInvalidArgument`
- `replica_count == 0` 返回 `kInvalidArgument`
- `minimum_successful_writes == 0` 返回 `kInvalidArgument`
- `minimum_successful_writes > replica_count` 返回 `kInvalidArgument`
- 候选列表为空返回 `kInvalidArgument`

### 4. 筛掉不合格候选

- 先把 `excluded_nodes` 转成 `unordered_set`，避免显式排除检查退化成线性扫描
- 然后遍历输入候选：
  - 合格节点进入 `eligible_candidates`
  - 不合格节点通过 `AddExclusion(...)` 记录到 `decision.excluded_nodes`

### 5. 对合格候选做稳定排序

- 排序优先级是：
  - `available_capacity_bytes` 更大优先
  - `load.TotalInflight()` 更低优先
  - `load.active_writes` 更低优先
  - `load.active_reads` 更低优先
  - `node_id` 字典序更小优先
  - `original_index` 更小优先
- 最后一层 `original_index` 兜底的作用是：
  - 当所有可观测指标都完全相同时，输出仍然稳定可预测

### 6. 两阶段选点

- `SelectReplicas()` 内部维护两个集合：
  - `selected_node_ids`
    - 防止重复选择相同 `node_id`
  - `selected_zones`
    - 在启用跨 zone 优先时，追踪哪些 zone 已被占用
- 内部 lambda `try_select(...)` 负责真正落选节点，主要规则：
  - 如果 `avoid_same_node=true` 且该 `node_id` 已被选过，直接跳过
  - 如果当前是“必须新 zone”阶段，但节点 zone 为空或 zone 已被占用，则跳过
  - 如果因为重复 `node_id` 被跳过，会写一条 `duplicate node_id skipped during selection` 到 `excluded_nodes`
  - 成功选中后，把节点追加到 `decision.replica_nodes`，并更新 `selected_node_ids` 和 `selected_zones`

### 7. `prefer_distinct_zones` 的行为

- 如果 `prefer_distinct_zones=true`：
  - 第一轮先遍历排好序的候选，只挑能提供新 zone 的节点
  - 同时向 `decision.reasons` 写入一条说明，表明启用了 zone spread 优先策略
- 第一轮结束后，无论是否选满，都会进入第二轮普通补齐流程

### 8. 普通补齐流程

- 第二轮再次按同一排序顺序遍历 `eligible_candidates`
- 这时不再要求必须来自新 zone，只要不违反重复节点约束即可
- 直到选满 `replica_count` 或候选耗尽

### 9. 失败和成功返回

- 如果最终 `replica_nodes.size() < replica_count`：
  - 返回 `kNodeUnavailable`
  - `error_detail` 为 `eligible storage nodes are fewer than requested replica_count`
  - 同时向 `decision.reasons` 追加一条“可用节点不足”的说明
- 如果成功选满：
  - 返回 `kOk`
  - 向 `decision.reasons` 追加一条排序依据说明：
    - `replicas are ordered by available capacity, lower inflight load, then node_id`

## 当前选择语义

- 只选择可写健康节点；当前 MVP 将 `kHealthy` 视为唯一可写状态
- 不选择：
  - `kDegraded` / `kReadOnly` / `kUnavailable` / `kDraining`
  - `disk_pressure = kHigh / kFull`
  - `write_admission_overloaded = true`
  - 可用容量不足以容纳 `chunk_size_bytes + reserve_capacity_bytes`
  - 调用方显式排除的节点
- 节点排序固定为：
  - `available_capacity_bytes` 更大优先
  - `load.TotalInflight()` 更低优先
  - `load.active_writes` 更低优先
  - `load.active_reads` 更低优先
  - `node_id` 字典序兜底
- 输出副本节点不会重复 `node_id`
- `prefer_distinct_zones = true` 时，会先尽量跨 zone 选点，再按常规排序补齐剩余副本

## 默认策略边界

- 默认 `replica_count = 3`
- 默认 `minimum_successful_writes = 2`
- 当前模块只负责“选谁”，不负责“写是否成功”
- 后续 upload coordinator 需要在至少 `minimum_successful_writes` 个节点 durable success 后，才允许 metadata commit

## 未实现内容

- `rack` spread 当前只是字段占位，还没有单独策略
- 没有真实 registry snapshot、staleness、failure cache、recent failure scoring
- 没有动态热点规避、局部性优先、跨 failure domain 复杂策略
- 没有把选择结果接到实际 `StorageNodeClient::WriteChunk` 调用
