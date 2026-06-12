# T065 Record Join API And Safety Validation

## Scope

本任务是 US4 Metadata/RaftNode dynamic join 的验证收口任务。

- 不写生产代码。
- 不修改测试逻辑。
- 只汇总 T055-T064 已实现的 join API、leader validation、AddLearner boundary、ViewNode non-authoritative boundary 和 quorum safety 验证结果。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t065-record-join-api-and-safety-validation.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## US4 Summary

- T055 已通过 `tests/node_identity_test.cpp` 和 `tests/cluster_config_test.cpp` 把 dynamic Metadata candidate 约束为本地 `candidate`/`joining` 身份，禁止通过本地 identity/config 直接成为 voter。
- T056/T059/T060 已建立 `JoinMetadataCluster` additive proto contract，并在 `modules/raft/service/metadata_service_impl.cpp` 实现 leader-only admission、request validation、`NOT_LEADER` + leader hint、duplicate/pending/conflict 映射。
- T061 已在 `apps/metadata_node_app.cpp` 接入 dynamic join candidate mode，保持 bootstrap voter 启动路径不退化。
- T062 已实现通过 ViewNode metadata candidates 和 leader hint 做 discovery，并在 `NOT_LEADER` 时继续按 leader hint 或下一个 discovered candidate 重试。
- T063 已在 `modules/raft/node/raft_node.cpp` 提供 `ProposeAddLearner()` 边界：leader-only、duplicate、conflict、pending membership change、invalid argument；返回 accepted/pending 但不写 committed learner membership。
- T064 已在 `modules/view/view_registry.cpp` 和 quorum 场景测试中确认：ViewNode metadata observation 只用于 discovery/diagnostics，不扩大 committed voter/learner membership，不改变 quorum。
- `MetadataClientScenarioTest.*` 现在覆盖 JoinMetadataCluster proto contract、CLI 不越权、ViewNode leader hint 优先、`NOT_LEADER` fallback 和全部失败时的明确报错。
- `IntegratedObjectStorageQuorumTest.*` 现在覆盖 duplicate join、pending membership change、follower authority reject、leader validation、AddLearner boundary，以及 ViewNode observed voter/joining candidate 不污染 committed membership 或 quorum。
- `ViewNodeDiscoveryTest.MetadataObservedRegistrationRemainsObservationOnlyAndRespectsMergeAndLiveness` 已覆盖 observed metadata registration 仍是 observation-only，并保持 merge/liveness 规则。
- 当前仓库状态仍停留在 validation/boundary 阶段；learner catch-up、InstallSnapshot catch-up、promote-to-voter、odd-voter-safe promotion 尚未实现。

## Current Join API Semantics

- dynamic Metadata candidate 不能通过本地 config / `identity_file` 成为 voter。
- `JoinMetadataCluster` 是 Metadata leader authority 路径。
- follower / non-leader 不能接受 join authority，只能返回 `NOT_LEADER` 和 leader hint。
- ViewNode discovery 只提供 observed MetadataNode candidates，不是 membership authority。
- `NOT_LEADER` / leader hint fallback 只用于寻找 Metadata leader，不改变 membership。
- `AddLearner` proposal path 当前只是 learner membership admission boundary，不等于 committed learner、catch-up 完成或 promote 完成。
- accepted / validation-passed / duplicate / pending-membership-change 都不等于 voter。
- committed Raft membership 仍是 membership 事实来源。
- 当前阶段不实现 learner catch-up / promote-to-voter。

## Safety Invariants

- 当前 join validation 不降低 quorum，quorum 仍按 committed voters 计算。
- 当前实现不绕过 Raft log / committed membership authority。
- duplicate join 不产生重复 learner / voter。
- pending membership change 不会被静默覆盖。
- ViewNode observation 不会自动成为 learner。
- ViewNode observation 不会自动成为 voter。
- validation failure 不污染 committed membership。
- dynamic candidate 不能本地声明 voter。
- bootstrap voter startup 路径保持原有语义，不被 dynamic join candidate mode 替代。

## Linux Validation

- Build command:

```bash
mkdir -p tmp/test-logs && (
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_metadata_client_scenario integrated_object_storage_quorum test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock > tmp/test-logs/t065-build.log 2>&1
```

- Build result: PASS
- Build log: `tmp/test-logs/t065-build.log`

- Test command:

```bash
ctest --preset debug-tests -R '^MetadataClientScenarioTest\.' --output-on-failure > tmp/test-logs/t065-metadata-client.log 2>&1
```

- Result: PASS
- Coverage: `14/14`
- Total time: `0.63 sec`
- Log: `tmp/test-logs/t065-metadata-client.log`

- Test command:

```bash
ctest --preset debug-tests -R '^IntegratedObjectStorageQuorumTest\.' --output-on-failure > tmp/test-logs/t065-integrated-quorum.log 2>&1
```

- Result: PASS
- Coverage: `8/8`
- Total time: `9.85 sec`
- Log: `tmp/test-logs/t065-integrated-quorum.log`

- Test command:

```bash
ctest --preset debug-tests -R '^ViewNodeDiscoveryTest\.' --output-on-failure > tmp/test-logs/t065-view-node.log 2>&1
```

- Result: PASS
- Coverage: `30/30`
- Total time: `0.30 sec`
- Log: `tmp/test-logs/t065-view-node.log`

- local RPC startup/status smoke: not run
- 原因：T065 目标是 US4 validation 收口；当前 targeted CTest 已覆盖 join API、leader validation、AddLearner boundary 和 ViewNode non-authoritative boundary，local RPC dynamic metadata add-node smoke 仍保留给后续阶段。

## Windows Validation

- Windows: pending / not run
- 原因：本任务未在 Windows 环境执行

## macOS Validation

- macOS: pending / not run
- 原因：本任务未在 macOS 环境执行；当前项目仍以 Linux 为主验证平台

## Skipped Checks

- local RPC runtime dynamic metadata join smoke: not run
- learner AppendEntries catch-up: not in this phase
- learner InstallSnapshot catch-up: not in this phase
- promote-to-voter: not in this phase
- odd-voter-safe promotion / batch promote: not in this phase
- Windows validation: pending / not run
- macOS validation: pending / not run

## Remaining Risks / Follow-ups

- learner AppendEntries catch-up 仍未实现或未验证。
- learner InstallSnapshot catch-up 仍未实现或未验证。
- promote-to-voter 仍未实现或未验证。
- odd-voter promotion / batch promote 仍未实现或未验证。
- learner lag diagnostics / ready-to-promote / waiting-for-pair 状态仍未实现或未验证。
- Windows/macOS 仍未验证，保持 pending。
- dynamic Metadata join 当前仍停留在 validation / AddLearner boundary，不是完整 membership lifecycle。
- local RPC runtime dynamic metadata add-node smoke 仍缺失。
- multi-ViewNode peer sync 对 metadata join discovery 的长期影响仍未完整验证。

## Result

- PASS
- `tasks.md`：已只勾选 T065
- 可以进入 T066
