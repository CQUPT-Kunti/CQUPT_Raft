# T061 Add Dynamic Join Mode Wiring In Metadata Node App

## Scope

本任务在 `apps/metadata_node_app.cpp` 接入 dynamic Metadata candidate 启动分支，使 candidate 模式不再沿用 bootstrap voter 启动路径，也不再试图本地直接成为 voter。当前只把 candidate 接到 `JoinMetadataCluster` validation 入口，不实现 AddLearner、learner catch-up、promote-to-voter，也不修改 committed membership / quorum / election。

## Task Source

- `tasks.md`: T061
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `modules/cluster/node_identity.h`
- `apps/metadata_node_app.cpp`
- `proto/metadata.proto`
- `modules/raft/service/metadata_service_impl.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t055-add-dynamic-metadata-candidate-identity-config-tests.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t060-implement-metadata-leader-join-validation.md`

## Files Changed

- `apps/metadata_node_app.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t061-add-dynamic-join-mode-wiring-in-metadata-node-app.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

### Metadata node app wiring

在 `metadata_node_app` 新增 dynamic join candidate 启动分支 helper：

- `BuildDynamicJoinSeedEndpoints(...)`
- `BuildJoinMetadataClusterRequest(...)`
- `AttemptDynamicJoinValidation(...)`
- `DescribeJoinAttempt(...)`
- `RunDynamicJoinCandidateMode(...)`

candidate 模式现在会：

- 复用本地 identity / process incarnation 边界
- 从静态 `metadata_nodes` 中收集非 candidate seed endpoint
- 构造 `JoinMetadataClusterRequest`
- 通过 `MetadataService::JoinMetadataCluster(...)` 进入 T060 的 leader validation 路径
- 对 `NOT_LEADER` 保留明确诊断并继续尝试其他静态 seed
- 对 `ACCEPTED_PENDING_COMMIT` / `DUPLICATE` / `PENDING_MEMBERSHIP_CHANGE` 明确返回 non-success，说明 validation 已进入但本任务不启动 learner / voter
- 对 invalid / rejected / transport failure 输出清晰错误

### Bootstrap voter path

bootstrap voter 分支保持原有逻辑不变：

- 继续加载或创建本地 identity
- 继续生成 process incarnation
- 继续构造 `RaftNode`
- 继续按原路径启动 metadata raft 节点

candidate 模式不会进入 bootstrap voter 初始 membership，也不会本地新建独立 bootstrap cluster。

## Boundary Checks

- 未实现 AddLearner
- 未实现 learner catch-up
- 未实现 promote-to-voter
- 未修改 committed Raft membership
- 未修改 Raft quorum / election
- 未修改 proto / 持久化格式
- 未让 dynamic candidate 本地成为 voter
- 未让 ViewNode 成为 membership authority
- candidate 模式未启动本地 `RaftNode`，避免 non-member 以 voter 身份错误启动

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target metadata_node_app cluster_config_test test_node_identity
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `ctest --preset debug-tests -R "cluster_config_|NodeIdentityTest\\." --output-on-failure`
  - Result: PASS
- Summary:
  - `metadata_node_app` targeted build: PASS
  - `cluster_config_*`: `19/19` PASS
  - `NodeIdentityTest.*`: `37/37` PASS
- Local RPC startup smoke:
  - `Not run`
  - 原因：本任务已有针对 `cluster_config` 和 `node_identity` 的 targeted 验证，且本次修改只涉及 app wiring；未额外扩展到 heavier local demo startup

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated
- Windows: not run, pending
- macOS: not run, pending

## Risks / Follow-ups

- 当前 candidate 模式只接到 leader validation，不表示 AddLearner 已完成
- 当前 candidate 模式只顺序尝试静态 metadata seed；基于 `leader_hint` 或 ViewNode candidates 的 fallback 留给 T062
- 当前 `ACCEPTED_PENDING_COMMIT` / `DUPLICATE` / `PENDING_MEMBERSHIP_CHANGE` 都会以 non-success 退出，避免 app 把 validation passed 误当成已获得 membership authority
- local RPC startup smoke 本任务未执行，如后续需要端到端证明 candidate startup UX，可在 T062/T063 结合 join flow 一并覆盖

## Result

PASS

- bootstrap voter startup 未退化
- dynamic candidate 已与 bootstrap voter 启动路径分离
- dynamic candidate 只能进入 `JoinMetadataCluster` validation 入口，不能本地成为 voter
- 可以进入 T062
