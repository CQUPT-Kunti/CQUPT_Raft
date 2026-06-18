# T089 Add Or Extend 009 Local RPC Topology Config

## Scope

- 任务类型：example 配置 / 文档 / 验证
- 本任务只为 Phase 10 local RPC 动态验证准备 topology config。
- 本任务不实现启动脚本、shutdown 脚本、StorageNode join 命令、Metadata learner join 命令，也不修改生产代码。

## Files Changed

- `examples/object-storage-local-009-dynamic/cluster.json`
- `examples/object-storage-local-009-dynamic/storage-join-store-7.json`
- `examples/object-storage-local-009-dynamic/metadata-learner-4.json`
- `examples/object-storage-local-009-dynamic/metadata-learner-5.json`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t089-add-or-extend-009-local-rpc-topology-config.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## Configuration Strategy

- 保持 `examples/object-storage-local-3meta-6store/cluster.json` 完全不动，作为 008 静态 baseline。
- 新增 sibling 目录 `examples/object-storage-local-009-dynamic/`，把 009 Phase 10 需要的 topology 输入独立出来，避免 T090-T098 之前直接改坏现有脚本。
- 所有 sibling config 统一使用：
  - `cluster_id=example-local-009-dynamic`
  - 双 ViewNode peer seed 拓扑
  - `3` 个 initial Metadata voters
  - discovery-only StorageNode
  - Metadata dynamic join 仅以 `initial_role=candidate` 表达后续脚本输入，不能本地写成 voter

## What Was Added

- `cluster.json`
  - Phase 10 初始 topology 基线
  - `2` 个 ViewNodes
  - `3` 个 initial Metadata voters
  - `6` 个初始 StorageNodes
- `storage-join-store-7.json`
  - 在初始 topology 基础上追加 `store-7`
  - 供后续 T092 作为运行中 StorageNode join 的 config 输入
- `metadata-learner-4.json`
  - 在初始 topology 基础上追加 `meta-4`
  - `meta-4` 以 `initial_role=candidate` 表达后续 learner join candidate 输入
- `metadata-learner-5.json`
  - 在初始 topology 基础上追加 `meta-5`
  - `meta-5` 同样只以 `initial_role=candidate` 表达后续第二个 learner candidate 输入

## Topology Semantics

- `2` ViewNodes 如何表达：
  - `view-1` -> `127.0.0.1:8301`
  - `view-2` -> `127.0.0.1:8302`
  - 双方互为 `peer_seeds`
- `3` initial Metadata voters 如何表达：
  - `meta-1..meta-3`
  - `raft_id=1..3`
  - `initial_role=voter`
  - `initial_raft_membership.voter_raft_ids=[1,2,3]`
- dynamic StorageNode join 如何表达：
  - `storage-join-store-7.json` 额外包含 `store-7`
  - `store-7` 只作为后续运行时启动输入，不进入任何 Raft membership 配置
- Metadata learner candidate 如何表达：
  - `metadata-learner-4.json` / `metadata-learner-5.json` 中新增的 `meta-4` / `meta-5` 均使用 `initial_role=candidate`
  - 它们不出现在 `initial_raft_membership.voter_raft_ids`
  - 也不出现在 `initial_raft_membership.learner_raft_ids`
  - 真正的 learner / voter authority 仍必须来自 Metadata leader 和 committed membership log

## Boundary Preservation

- 008 baseline：保留
  - 未修改 `examples/object-storage-local-3meta-6store/cluster.json`
  - 未修改现有 `qidong.sh` / `tingzhi.sh` / `rpc_demo.sh`
- ViewNode 不成为 membership authority：
  - `peer_seeds` 只用于 ViewNode discovery / observed registry sync 输入
  - `initial_raft_membership` 仍只包含 metadata voter raft ids
- StorageNode dynamic join 仍是 discovery-only：
  - `store-7` 不进入 `initial_raft_membership`
  - 未表达任何旧对象 rebalance 语义
- dynamic Metadata 节点不本地变成 voter：
  - `meta-4` / `meta-5` 只配置为 `candidate`
  - 未把 candidate 写进 voter/learner 初始 committed membership
- schema 边界保持一致：
  - 仅使用当前 schema 已支持字段：`cluster_id`、`base_dir`、`view_nodes`、`peer_seeds`、`metadata_nodes`、`raft_id`、`initial_role`、`storage_nodes`、`data_dir`、`snapshot_dir`、`endpoint`
  - 没有硬塞当前 schema 不支持的 `identity_file` 或非法动态 join 字段

## Validation

- JSON 结构检查：
  - `examples/object-storage-local-009-dynamic/cluster.json`: PASS
  - `examples/object-storage-local-009-dynamic/storage-join-store-7.json`: PASS
  - `examples/object-storage-local-009-dynamic/metadata-learner-4.json`: PASS
  - `examples/object-storage-local-009-dynamic/metadata-learner-5.json`: PASS
- build 命令：
  - `( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client cluster_config_test ) 9>/tmp/cqupt_raft_build.lock`
- ctest 命令：
  - `ctest --preset debug-tests -R "cluster_config|ClusterConfig" --output-on-failure`
- 结果：`PASS`
- 通过摘要：
  - build：`PASS`
  - `ctest`：`20/20` PASS
  - 总耗时：`0.11 sec`
- 日志路径：
  - `tmp/test-logs/t089-build.log`
  - `tmp/test-logs/t089-ctest.log`

## Relevant Validation Evidence

- baseline compatibility:
  - `cluster_config_validation_test.parses_single_view_config_without_peer_seeds_and_keeps_baseline_compatibility`
- dual ViewNode peer seeds:
  - `cluster_config_validation_test.parses_multi_view_peer_seeds_and_keeps_initial_membership_unchanged`
- candidate config legality:
  - `cluster_config_validation_test.allows_metadata_candidate_outside_initial_membership_and_roundtrips_json`
  - `cluster_config_validation_test.rejects_metadata_candidate_that_stays_in_initial_membership`
  - `cluster_config_validation_test.rejects_metadata_candidate_that_attempts_local_voter_role`
- unified app config parsing smoke:
  - `IntegratedObjectStorageE2ETest.AppConfigParsingSmokeResolvesViewMetadataStorageAndClientBootstrapFromUnifiedClusterConfig`

## Skipped

- 2-ViewNode startup script smoke：not run
  - 原因：T090 尚未实现，不把脚本级双 ViewNode 启动作为 T089 完成前置
- shutdown script smoke：not run
  - 原因：T091 尚未实现
- runtime StorageNode join command smoke：not run
  - 原因：T092 尚未实现
- runtime Metadata learner join command smoke：not run
  - 原因：T093/T094 尚未实现

## Result

- 最终状态：`PASS`
- 是否已勾选 `T089`：是
- 是否可以进入 `T090`：可以
