# T042 Per-Node Config Resolution 报告

## 1. 修改了哪些文件

- `modules/cluster/cluster_config.cpp`
- `modules/cluster/cluster_config.h`
- `modules/cluster/module-notes.md`
- `tests/cluster_config_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t042-per-node-config-resolution.md`

未修改：

- `proto/*`
- app 入口
- `common-risk-notes.md`
- `risk-register.md`

## 2. per-node config resolution 和 endpoint allocation 做了什么

本任务只实现配置解析与 endpoint 分配逻辑，没有实现 app startup、CLI generate-config 或真实节点启动。

本次新增/调整的核心能力：

- 在 `cluster_config.h` 中新增 `ClusterEndpointAssignment` / `ClusterEndpointAllocationResult`
  - 对统一 generation request 生成 ViewNode / MetadataNode / StorageNode 的稳定 endpoint 分配结果
  - 返回可诊断的 `status` / `validation` / `error_detail`
- 新增 `AllocateClusterEndpoints(const ClusterConfigGenerationRequest &request)`
  - 根据 role、ordinal、固定 node_id override 和端口基线生成 endpoint
  - 对重复 endpoint、非法 endpoint、请求参数不合法等场景返回明确诊断
  - 不会静默跳到其它端口或创建默认 demo 拓扑
- 在 `cluster_config.h` 中新增 `ResolvedClusterNodeConfig` / `ClusterNodeResolutionResult`
  - 承载单节点启动前所需的解析结果
  - 可表达 `endpoint`、`data_dir`、`snapshot_dir`、`raft_id`、`initial_role`、`capacity_bytes` 等字段
- 新增 `ResolveClusterNodeConfig(const ClusterConfig &, ClusterNodeType, std::string_view)`
  - 按 `node_type + node_id` 从完整 `ClusterConfig` 中精确解析单个节点配置
  - 若 `node_id` 不存在、role 不匹配、或原始 cluster config 已无效，则返回明确错误
  - 不允许 fallback 到“第一个节点”或任何默认节点
- `GenerateDeterministicClusterConfig(...)` 改为复用 `AllocateClusterEndpoints(...)`
  - 避免 endpoint 分配逻辑在多个位置分叉
  - 保持相同输入下的稳定、可重复输出

## 3. 是否保持配置驱动、无硬编码节点拓扑

已保持。

- endpoint 分配仍完全来自 `ClusterConfigGenerationRequest`
- node_id 可由固定 override 或稳定默认规则生成
- 没有引入固定 demo 拓扑选择逻辑
- 没有静默补节点、改端口、改 role 或覆盖冲突配置
- 没有修改 Raft quorum / membership / election / commit 生产语义

## 4. 是否发现不合理点 / 警告 / 风险

发现的注意点：

- 当前 `ResolveClusterNodeConfig(...)` 返回的是统一的解析结果对象，而不是直接拆成后续 app 的专用启动配置类型；这能满足 T042 的解析边界，但真正的 app 启动参数装配仍建议在 T045/T046/T047 中按各自角色继续收敛。
- endpoint allocation 目前只做“分配 + 诊断”，没有引入更复杂的端口保留/探测机制；这是有意保持边界，避免在 T042 提前实现真实启动时才需要的运行时行为。
- 当前工作区在本任务开始前已存在其他未提交改动；本次只把 `T042` 从 `[ ]` 改为 `[X]`，如 `git diff` 中同时出现 `T041`、`T044` 等任务状态变化，应视为既有脏改动而非本任务新增。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md` 或 `risk-register.md`。

## 6. 验证命令和结果

执行的验证命令：

```bash
git diff -- modules/cluster/cluster_config.cpp modules/cluster/cluster_config.h modules/cluster/module-notes.md tests/cluster_config_test.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t042-per-node-config-resolution.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target cluster_config_test'
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R cluster_config --output-on-failure'
```

结果：

- `cluster_config_test` 构建通过
- `ctest --test-dir build/linux/safe -R cluster_config --output-on-failure`
  - 7/7 PASS
  - 通过用例：
    - `cluster_config_generation_test.supports_1_3_5_7_voter_layouts_with_valid_generated_membership`
    - `cluster_config_generation_test.same_request_generates_reproducible_config_without_hardcoded_demo_topology`
    - `cluster_config_validation_test.rejects_zero_storage_capacity_in_generated_config`
    - `cluster_config_endpoint_allocation_test.allocates_stable_role_specific_endpoints_from_request`
    - `cluster_config_endpoint_allocation_test.reports_duplicate_endpoint_conflicts_from_overlapping_port_ranges`
    - `cluster_config_resolution_test.resolves_view_metadata_and_storage_nodes_by_role_and_node_id`
    - `cluster_config_resolution_test.rejects_missing_node_and_role_mismatch_without_fallback`

本地日志文件：

- `tmp/test-logs/t042-build.log`
- `tmp/test-logs/t042-ctest-safe.log`
