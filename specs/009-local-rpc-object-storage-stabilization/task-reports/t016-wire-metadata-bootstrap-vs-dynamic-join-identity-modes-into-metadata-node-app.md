# T016 Wire Metadata Bootstrap vs Dynamic Join Identity Modes into `metadata_node_app`

## Scope

本任务只处理 MetadataNode 启动期的 identity wiring：

- bootstrap initial voter 的 load/create + restart validation
- dynamic join candidate 的 load/create + boundary reject
- process incarnation 接入

本任务不实现 JoinMetadataCluster、AddLearner、learner catch-up、promote-to-voter，也不修改已提交的 Raft membership authority。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T016
- `specs/009-local-rpc-object-storage-stabilization/contracts/identity-lifecycle.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/metadata-learner-join.md`
- `specs/009-local-rpc-object-storage-stabilization/data-model.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`

## Files Changed

- `apps/metadata_node_app.cpp`
- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `tests/cluster_config_test.cpp`

## What Changed

- 在 `cluster_config` 中为 Metadata `initial_role` 增加 `candidate`，并要求 candidate 不能出现在 `initial_raft_membership` 的 voter / learner 集合里。
- 在 `metadata_node_app` 中区分 bootstrap voter / learner 与 dynamic join candidate 三种启动角色，并显式写入 `NodeIdentity.membership_state`。
- bootstrap voter / learner 继续走统一 `LoadOrCreateNodeIdentity(...)`，并对已存在 identity 的 `cluster_id`、`node_type`、`node_id`、`raft_id`、`membership_state` 做严格校验。
- 在 durable identity 成功后统一接入 `CreateProcessIncarnation(...)`。
- 对 dynamic join candidate 只做 durable identity + process incarnation 准备，然后在进入 `RaftNode` 之前明确返回 `kUnsupported`，拒绝本地把 candidate 当成 voter / learner 启动。
- 补充 `cluster_config_test`，覆盖 candidate 不在初始 committed membership 时可通过，以及 candidate 仍留在初始 membership 时必须拒绝。

## Boundary Checks

- 没有实现 JoinMetadataCluster RPC。
- 没有实现 AddLearner。
- 没有实现 learner catch-up。
- 没有实现 promote-to-voter。
- 没有修改 quorum 计算逻辑。
- 没有让 ViewNode 成为 membership authority。
- 没有允许 dynamic join candidate 通过本地 `identity_file` 自行晋升为 voter。

## Validation

- 文本检查：
  - 阅读并对照 `apps/metadata_node_app.cpp`、`modules/cluster/node_identity.*`、`modules/cluster/cluster_config.*`
  - 用 `ctest --preset debug-tests -N` 确认真实 CTest 入口；`debug-ninja-low-parallel` 仅存在 build preset，不存在 test preset
- 构建命令：
  - `cmake --build --preset debug-ninja-low-parallel --target metadata_node_app test_node_identity cluster_config_test`
  - 结果：PASS
- targeted CTest：
  - `ctest --preset debug-tests -R "NodeIdentityTest\\.|cluster_config_" --output-on-failure`
  - 结果：PASS
  - 摘要：`100% tests passed, 0 tests failed out of 48`
  - 日志：`tmp/test-logs/t016-ctest.log`
- optional local RPC status smoke：
  - 先构建：`cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client`
  - 再执行：
    - `examples/object-storage-local-3meta-6store/tingzhi.sh`
    - 清理旧运行数据：
      - `rm -rf examples/object-storage-local-3meta-6store/logs`
      - `rm -rf examples/object-storage-local-3meta-6store/pids`
      - `rm -rf examples/object-storage-local-3meta-6store/nodes/*/data`
      - `rm -rf examples/object-storage-local-3meta-6store/nodes/meta-*/snapshots`
    - `examples/object-storage-local-3meta-6store/qidong.sh`
    - `examples/object-storage-local-3meta-6store/rpc_demo.sh status`
    - `examples/object-storage-local-3meta-6store/tingzhi.sh`
  - 结果：PASS
  - 摘要：
    - `status OK`
    - `view_nodes=1 metadata_nodes=3 storage_nodes=6`
    - `leader_hint.node_id=meta-1 raft_id=1`
  - 日志：
    - `tmp/test-logs/t016-clean-and-status.log`

## Build Lock

- targeted build 使用了 `flock` 构建锁，并成功获得锁。
- targeted CTest 使用了同一个 `flock` 锁文件。
- optional smoke 前的构建使用了同一个 `flock` 锁文件。

## Platform Notes

- Linux：已完成代码修改、targeted build、targeted CTest，并在清理旧运行数据后通过 local RPC status smoke。
- Windows：未实测，pending。

## Risks / Follow-ups

- 示例目录中的旧 `node.identity`/snapshot 运行数据会在新 identity 校验下 fail-fast；后续若要做 example smoke，默认应先停机并清理旧运行数据再验证。
- dynamic join candidate 目前是安全最小 wiring：identity/incarnation 已接入，但在进入 Raft 前显式拒绝启动；后续真正的 learner / voter 过渡仍需 T017 之后的 membership 任务完成。

## Result

- 结果：PASS
- 已完成 T016 要求的 Metadata identity mode wiring，并通过 targeted build + targeted CTest。
- 清理旧运行数据后，现有 3 voter bootstrap local RPC status smoke 通过，确认没有破坏 008 baseline 的启动路径。
- 可以进入 T017。
