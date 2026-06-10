# T011 Cluster Config 校验与确定性生成报告

## 1. 修改了哪些文件

- `modules/cluster/cluster_config.cpp`
- `modules/cluster/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t011-cluster-config-validation-generation.md`

`modules/cluster/cluster_config.h` 未修改。

## 2. cluster config 校验和确定性生成做了什么

### 2.1 实现了哪些接口

本次在 `modules/cluster/cluster_config.cpp` 中实现了：

- `ValidateClusterConfig`
- `ValidateInitialRaftMembership`
- `GenerateDeterministicClusterConfig`
- `ComputeInitialRaftQuorumSize(...)`
- `ToString(...)`
- `DescribeClusterConfigIssue(...)`

### 2.2 配置校验覆盖范围

`ValidateClusterConfig` 当前覆盖了以下边界：

- `cluster_id`、`base_dir` 非空校验
- ViewNode / MetadataNode / StorageNode 至少各有一个节点
- endpoint 格式必须为 `host:port`，端口范围必须在 `1..65535`
- `node_id` 字符合法性校验
- 全集群 `node_id` 冲突检查
- 全集群 endpoint 冲突检查
- 各节点 `data_dir` 非空与共享路径冲突检查
- MetadataNode `snapshot_dir` 非空与路径冲突检查
- MetadataNode `raft_id` 正数与重复检查
- StorageNode `capacity_bytes > 0`
- `chunk_policy` 的 `chunk_size_bytes`、`replica_count`、`minimum_successful_writes`、`checksum_algorithm` 基本边界
- timeout 配置必须为正，并校验：
  - `liveness_stale_timeout > heartbeat_interval`
  - `liveness_dead_timeout > liveness_stale_timeout`

`ValidateInitialRaftMembership` 当前覆盖了以下边界：

- `membership_epoch > 0`
- voter 集合非空且必须是奇数个，满足 1/3/5/7 风格 quorum 布局
- voter / learner `raft_id` 必须为正
- voter / learner 内部不能重复
- 同一个 `raft_id` 不能同时出现在 voter 和 learner
- membership 中的 `raft_id` 必须属于已配置的 MetadataNode
- MetadataNode 的 `initial_role` 必须与 membership 中的 voter / learner 分配一致
- 每个 MetadataNode 必须恰好出现一次

### 2.3 确定性配置生成逻辑

`GenerateDeterministicClusterConfig` 当前实现了稳定、可重复的配置展开：

- 相同输入生成相同 `ClusterConfig`
- 节点数量完全由请求中的 `view_node_count`、`metadata_node_count`、`storage_node_count` 驱动，不依赖代码常量补节点
- 默认 node_id 稳定生成：
  - `view-1`, `view-2`, ...
  - `meta-1`, `meta-2`, ...
  - `store-1`, `store-2`, ...
- 默认 `raft_id` 稳定生成，并跳过已显式固定的正数 `raft_id`
- endpoint 按 `*_port_base + index` 稳定展开
- 目录按角色稳定展开：
  - ViewNode: `base_dir/view/<node_id>`
  - MetadataNode data: `base_dir/metadata/<node_id>/data`
  - MetadataNode snapshot: `base_dir/metadata/<node_id>/snapshots`
  - StorageNode: `base_dir/storage/<node_id>`
- 前 `metadata_voter_count` 个 MetadataNode 标记为 voter，其余标记为 learner
- 初始 membership 从生成出的 MetadataNode 顺序稳定派生

### 2.4 明确返回错误而不是静默修正

本次实现对关键输入坚持显式报错，包括：

- node 数量为 0
- `metadata_voter_count` 为 0、超过 MetadataNode 总数或为偶数
- fixed id / fixed raft id / capacity override 数量超过节点数
- port base 范围溢出 `65535`
- `bind_host` 为空
- `advertise_host` 与 `bind_host` 不一致

其中 `bind_host` / `advertise_host` 的处理特意没有做静默回退：

- 当前 `ClusterConfig` 只有单一 `endpoint` 字段，不能安全表达独立的监听地址和对外地址
- 因此生成器要求 `advertise_host` 为空或与 `bind_host` 一致

## 3. 是否发现不合理点 / 警告 / 风险

发现一个接口层面的现实约束：

- `ClusterConfigGenerationRequest` 同时提供了 `bind_host` 和 `advertise_host`
- 但当前 `ClusterConfig` / `*NodeConfig` 只有单一 `endpoint` 字段
- 这意味着当前模型无法无歧义地同时保存 listen endpoint 和 advertise endpoint

本次处理方式：

- 没有静默选择其一
- 而是在两者不一致时返回明确错误
- 并把这条误用边界补充到了 `modules/cluster/module-notes.md`

另外一个既有情况是：

- 当前还没有 T040 的专门 cluster config generation tests
- 本次主要通过 configure/build 验证接入未破坏构建
- 后续应由 T040 补齐 1/3/5/7 voter 配置生成测试

## 4. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

- `common-risk-notes.md` 未修改
- `risk-register.md` 未修改

原因：

- 本任务范围限定为 cluster config validation / deterministic generation 逻辑
- 本次发现的是接口表达约束，已通过显式报错和模块说明收口，暂不需要额外登记风险文档

## 5. 验证命令和结果

执行命令：

```bash
git diff -- modules/cluster/cluster_config.cpp modules/cluster/cluster_config.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t011-cluster-config-validation-generation.md
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

结果：

- `git diff`：本次修改面集中在 `cluster_config.cpp`、`module-notes.md`、`T011` 任务状态和任务报告；`tasks.md` 中还包含本次开始前已存在的 `T016` 未提交勾选变更，需要与本任务区分查看
- `cmake --preset debug-ninja-low-parallel`：PASS，耗时约 8 秒
- `cmake --build --preset debug-ninja-low-parallel`：PASS，耗时约 49 秒

补充说明：

- 当前还没有 T040 的专门 cluster config generation tests；本次至少确认了 configure/build 不因 T011 失败
- configure 阶段仍出现现有 `tests/CMakeLists.txt` 中 `FetchContent_Declare` 相关 CMake dev warning，但未影响 T011 通过，且不是本次 cluster config 实现引入的新问题
