# T010 任务报告：cluster config 接口与数据结构边界

## 1. 修改了哪些文件

- `modules/cluster/cluster_config.h`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t010-cluster-config-interface.md`

说明：

- `modules/cluster/module-notes.md` 未修改；现有模块说明与本次接口边界一致。
- 当前工作区里 `modules/cluster/module-notes.md` 与 `tasks.md` 存在其他未提交改动；本任务没有回退或整理这些既有修改。

## 2. cluster_config.h 定义了哪些配置结构和接口边界

本次新建了 `modules/cluster/cluster_config.h`，只定义类型、接口和必要中文注释，没有实现任何具体配置生成、加载或校验逻辑。

### 2.1 基础类型与枚举

- `ClusterId`
- `ClusterNodeId`
- `ClusterNodeType`
  - `kView`
  - `kMetadata`
  - `kStorage`
- `MetadataNodeInitialRole`
  - `kVoter`
  - `kLearner`
- `ClusterChecksumAlgorithm`
  - 当前先预留 `kSha256`

### 2.2 配置错误与结果表达

- `ClusterConfigIssueCode`
  - 覆盖：
    - 缺失 `cluster_id`
    - 无效节点数量
    - 无效/重复 `node_id`
    - 无效/重复 endpoint
    - 缺失 `data_dir` / `snapshot_dir`
    - `data_dir` 冲突
    - capacity 非法
    - chunk policy / timeout policy 非法
    - `raft_id` 非法或重复
    - 初始 membership 非法
    - identity/config mismatch
    - durability mode 不支持

- `ClusterConfigStatusCode`
  - `kOk`
  - `kInvalidArgument`
  - `kConflict`
  - `kUnsupported`
  - `kInternalError`

- `ClusterConfigValidationIssue`
  - `code`
  - `field_path`
  - `message`
  - `node_type`
  - `node_id`
  - `endpoint`
  - `path`

- `ClusterConfigValidationResult`
  - `issues`
  - `ok()`

- `ClusterConfigGenerationResult`
  - `status`
  - `error_detail`
  - `config`
  - `validation`
  - `ok()`

这部分把“可诊断 validation result / error 表达边界”明确下来了，后续 T011 可以直接往这些结构里填充实现结果。

### 2.3 配置结构

- `FailureDomainConfig`
  - `zone`
  - `rack`

- `ChunkPolicyConfig`
  - `chunk_size_bytes`
  - `replica_count`
  - `minimum_successful_writes`
  - `checksum_algorithm`

- `ClusterTimeoutConfig`
  - `discovery_rpc_timeout`
  - `metadata_rpc_timeout`
  - `storage_rpc_timeout`
  - `heartbeat_interval`
  - `registration_timeout`
  - `commit_deadline`
  - `liveness_stale_timeout`
  - `liveness_dead_timeout`

- `ViewNodeConfig`
  - `node_id`：`std::optional<ClusterNodeId>`
  - `endpoint`
  - `data_dir`

- `MetadataNodeConfig`
  - `node_id`
  - `raft_id`
  - `endpoint`
  - `data_dir`
  - `snapshot_dir`
  - `initial_role`

- `StorageNodeConfig`
  - `node_id`：`std::optional<ClusterNodeId>`
  - `endpoint`
  - `data_dir`
  - `capacity_bytes`
  - `failure_domain`

- `InitialRaftMembershipConfig`
  - `voter_raft_ids`
  - `learner_raft_ids`
  - `membership_epoch`

- `ClusterConfig`
  - `cluster_id`
  - `base_dir`
  - `view_nodes`
  - `metadata_nodes`
  - `storage_nodes`
  - `initial_raft_membership`
  - `chunk_policy`
  - `timeouts`

### 2.4 配置生成输入接口

- `ClusterConfigGenerationRequest`
  - `cluster_id`
  - `base_dir`
  - `bind_host`
  - `advertise_host`
  - `view_node_count`
  - `metadata_node_count`
  - `metadata_voter_count`
  - `storage_node_count`
  - `view_port_base`
  - `metadata_port_base`
  - `storage_port_base`
  - `default_storage_capacity_bytes`
  - `chunk_policy`
  - `timeouts`
  - `fixed_view_node_ids`
  - `fixed_metadata_node_ids`
  - `fixed_metadata_raft_ids`
  - `fixed_storage_node_ids`
  - `storage_capacity_overrides_bytes`
  - `generation_seed`

这部分为 T011 的 deterministic generation 留出了稳定输入边界，但没有规定具体展开算法。

### 2.5 接口声明

- `ValidateClusterConfig(const ClusterConfig &config)`
- `ValidateInitialRaftMembership(const ClusterConfig &config)`
- `GenerateDeterministicClusterConfig(const ClusterConfigGenerationRequest &request)`
- `ComputeInitialRaftQuorumSize(std::size_t voter_count)`
- `ComputeInitialRaftQuorumSize(const InitialRaftMembershipConfig &membership)`
- `ToString(...)`
- `DescribeClusterConfigIssue(const ClusterConfigValidationIssue &issue)`

这些接口只声明，不实现。

## 3. 是否保持只定义接口、不实现生成/校验逻辑

- 是。
- 本次只新增头文件定义，没有新增 `.cpp` 实现。
- 没有实现：
  - endpoint 分配
  - `data_dir` / `snapshot_dir` 展开
  - 配置校验逻辑
  - deterministic generation 逻辑
  - membership 合法性判断
  - quorum 计算逻辑
  - `ToString(...)` / `DescribeClusterConfigIssue(...)` 的函数体

也没有提前实现 T012 / T013 的 `node.identity` 持久化逻辑。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前仓库里 `modules/cluster/` 还没有既有命名空间约定，本次新建头文件采用了独立 `clusterdemo` 命名空间，和现有 `raftdemo` / `storedemo` 的模块化风格一致。
- `ViewNodeConfig` 和 `StorageNodeConfig` 的 `node_id` 使用 `std::optional`，是为了保留“配置可留空，后续由配置生成器或 identity 流程补全”的边界；`MetadataNodeConfig` 仍然要求显式 `node_id + raft_id`，因为其身份更敏感。
- `ClusterChecksumAlgorithm` 当前只预留到 `kSha256`，后续如需扩展算法，应同步检查 upload / transfer / metadata manifest 的 checksum 约束，避免配置层先跑到实现层前面。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次只补接口与类型边界，没有改变已有行为，也没有引入新的风险类别。

## 6. 验证命令和结果

### 验证命令

```bash
git diff -- modules/cluster/cluster_config.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t010-cluster-config-interface.md
git status --short -- modules/cluster/cluster_config.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t010-cluster-config-interface.md
printf '#include "cluster/cluster_config.h"\nint main() { return 0; }\n' | c++ -std=c++20 -I modules -x c++ -fsyntax-only -
```

### 验证结果

- `git diff -- ...` 展示了三类内容：
  - 本任务新增的 `modules/cluster/cluster_config.h`
  - `tasks.md` 中 T010 从 `[ ]` 更新为 `[X]`
  - 工作区里原本已存在、但不属于本任务的其他修改：
    - `modules/cluster/module-notes.md` 的既有变更
    - `tasks.md` 中 T012 的既有勾选变化
- `git status --short -- ...` 将会确认：
  - `M modules/cluster/module-notes.md`
  - `M specs/008-integrated-object-storage-system/tasks.md`
  - `?? modules/cluster/cluster_config.h`
  - `?? specs/008-integrated-object-storage-system/task-reports/t010-cluster-config-interface.md`
- 结合本次实际编辑范围，可以确认：
  - T010 只新增了 `cluster_config.h`
  - 只把 `tasks.md` 中 T010 改为 `[X]`
  - 没有主动修改 `modules/cluster/module-notes.md`
- `printf ... | c++ -std=c++20 -I modules -x c++ -fsyntax-only -` 实际执行成功，说明：
  - 头文件语法有效
  - `#pragma once`、include、命名空间和声明形式都没有明显问题

## 结论

- T010 已完成。
- 当前可以进入 T011。
