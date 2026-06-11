# T017 Document Platform Durability Behavior For Identity Atomic Publish

## Scope

本任务是 Phase 2 identity lifecycle 的收口文档任务。

- 汇总 T006-T016 已完成的 identity 测试、实现和 app wiring
- 统一记录当前 `node.identity` 新格式、first-start、restart validation、process incarnation、atomic publish 与平台 durability 边界

本任务不写生产代码，不改测试逻辑，不修改 proto / CMake / example 脚本。

## Task Source

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`: T017
- `specs/009-local-rpc-object-storage-stabilization/task-reports/task-report-template.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/identity-lifecycle.md`
- `specs/009-local-rpc-object-storage-stabilization/data-model.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`
- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`
- `apps/storage_node_app.cpp`
- `apps/view_node_app.cpp`
- `apps/metadata_node_app.cpp`
- Phase 2 identity 相关报告：
  - `t006-add-storagenode-first-start-identity-creation-tests.md`
  - `t007-add-viewnode-first-start-identity-creation-and-restart-reuse-tests.md`
  - `t008-add-metadata-bootstrap-voter-identity-tests.md`
  - `t009-add-metadata-dynamic-join-candidate-identity-tests.md`
  - `t010-add-mismatch-and-corrupt-identity-fail-fast-tests.md`
  - `t011-extend-identity-data-model-for-node-type-optional-raft-id-membership-state-and-persistent-generation.md`
  - `t011-fix-remove-legacy-v1-nodeidentity-compatibility-and-align-docs-to-new-only-identity-format.md`
  - `t012-add-atomic-first-start-identity-creation-and-restart-validation.md`
  - `t013-add-process-incarnation-boot-epoch-generation-boundary.md`
  - `t014-wire-storagenode-identity-load-create-into-storage-node-app.md`
  - `t015-wire-viewnode-identity-load-create-into-view-node-app-before-self-registration.md`
  - `t016-wire-metadata-bootstrap-vs-dynamic-join-identity-modes-into-metadata-node-app.md`

说明：

- 虽然 `tasks.md` 中 T017 仍写的是 `phase-02-identity.md`，当前项目报告命名规则已经要求按任务名称命名，因此本报告使用 `t017-document-platform-durability-behavior-for-identity-atomic-publish.md`。
- `t011-extend-...` 是历史报告，里面仍保留已被 `t011-fix-...` 推翻的旧兼容结论；当前 Phase 2 收口以 `t011-fix-...`、现有 `node_identity.*` 代码和后续 T012-T016 结果为准。

## Files Changed

- `specs/009-local-rpc-object-storage-stabilization/task-reports/t017-document-platform-durability-behavior-for-identity-atomic-publish.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`

未修改：

- 生产代码
- 测试代码
- proto
- CMake
- example 脚本

## Identity Format Decision

- 009 尚未正式部署旧 identity 格式。
- 009 只支持当前 `NodeIdentity` 新格式，`identity_version` 仅接受当前版本。
- 不支持 legacy v1 compatibility。
- 不支持自动迁移、自动升级、静默补字段。
- `identity_file` 缺失才是 first-start 创建入口。
- existing `identity_file` 若为 old-format / corrupt / unknown-format / missing required fields，必须 fail-fast。
- corrupt / old-format identity 不能当作 missing identity 重新创建。
- mismatch identity 不能自动覆盖，也不能静默重解释 authority 语义。

## Completed Phase 2 Work Summary

- T006：补齐 StorageNode first-start identity 创建测试；报告显示 `NodeIdentityTest.*` 定向验证通过。
- T007：补齐 ViewNode first-start / restart reuse 测试；报告显示 `NodeIdentityTest.*` 定向验证通过。
- T008：补齐 Metadata bootstrap voter identity 测试；报告显示 `NodeIdentityTest.* + cluster_config_*` 定向验证通过。
- T009：补齐 Metadata dynamic join candidate identity 测试；报告显示 `NodeIdentityTest.*` 定向验证通过。
- T010：补齐 mismatch / corrupt / old-format fail-fast 测试；报告显示 `NodeIdentityTest.*` 定向验证通过。
- T011：扩展 `NodeIdentity` 数据模型，引入 `node_type`、optional `raft_id`、`membership_state`、`persistent_generation`。
- T011-fix：移除 legacy v1 compatibility，明确 new-only schema，旧格式缺字段 fail-fast。
- T012：实现 first-start 原子创建、staging publish、restart validation、create-only / replace-only 边界。
- T013：实现 `ProcessIncarnation` / boot epoch 边界，确保无效 identity 不产生可用 incarnation。
- T014：StorageNode app 接入 durable identity load/create + process incarnation，不把 StorageNode 注册写进 Raft authority。
- T015：ViewNode app 在 self registration 前接入 durable identity load/create + process incarnation，不把 ViewNode 变成 identity authority。
- T016：MetadataNode app 区分 bootstrap voter 与 dynamic join candidate，两者都接入 durable identity + process incarnation；candidate 模式显式拒绝越权启动成 voter / learner。

## Current Identity Semantics

### StorageNode

- first-start 若 `node.identity` 缺失，会本地创建 storage identity。
- restart 必须复用长期 `node_id`。
- 每次进程启动都生成新的 `ProcessIncarnation`。
- StorageNode 不携带 Metadata `raft_id`。
- StorageNode identity 不进入 Raft log。
- StorageNode identity 不影响 quorum、election 或 committed membership。

### ViewNode

- first-start 若 `node.identity` 缺失，会本地创建 view identity。
- restart 必须复用长期 `node_id`。
- 每次进程启动都生成新的 `ProcessIncarnation`。
- self registration 前必须先完成 durable identity load/create 和 validation。
- ViewNode 不是 identity authority。
- ViewNode 不是 Raft membership authority。

### Metadata Bootstrap Voter

- bootstrap 配置提供固定 `node_id` / `raft_id` / `initial_role`。
- durable identity 可持久化 `membership_state=voter`。
- restart 时必须校验 `cluster_id`、`node_type`、`node_id`、`raft_id`、`membership_state`。
- voter 身份由 bootstrap 配置与 committed membership 边界决定，不由 ViewNode 决定。

### Metadata Dynamic Join Candidate

- first-start 只创建 joining / candidate identity。
- `raft_id` 在当前实现中可为空或本地 provisional；无 committed authority 时不能把它解释成 voter authority。
- candidate 不能靠本地 `identity_file` 持久化成 voter。
- 后续成为 learner / voter 必须通过 Metadata leader + committed Raft membership log。
- 当前阶段只完成 identity mode wiring，不实现 `JoinMetadataCluster` / `AddLearner` / promote-to-voter。

## Process Incarnation Boundary

- `node_id` 是长期逻辑身份。
- `incarnation_id / boot epoch` 是单次进程启动身份。
- restart 后 `node_id` 不变，`incarnation_id` 改变。
- invalid / corrupt / mismatch identity 不生成可用 incarnation。
- `startup_sequence_base` 当前从 `kProcessIncarnationInitialSequence=1` 起步。
- `observed_time` 只用于 TTL / liveness / diagnostics，不能替代 incarnation 排序。
- incarnation 不参与 quorum，不改变 `raft_id`，也不改变 committed membership。

## Atomic Publish And Restart Validation

- `identity_file` 缺失时，`LoadOrCreateNodeIdentity(...)` 先 `LoadNodeIdentity(...)`，仅在 `NotFound` 且不是 `require_existing` 场景下进入创建。
- identity 持久化使用 staging 文件，文件名带 `.tmp.<pid>.<timestamp>` 后缀。
- Linux 路径：
  - 写 staging 文件
  - `fsync(fd)`
  - create-only 用 `link(staging, final)` 再 `unlink(staging)`
  - replace-only 用 `rename(staging, final)`
  - required durability 下再 `fsync(data_dir)`
- Windows 路径：
  - 写 staging 文件
  - `FlushFileBuffers(handle)`
  - `MoveFileExW(...)` publish
  - 若 durability 是 `kRequired`，因为目录 durability 未实现，明确返回错误而不是 silent success
  - `kBestEffortForTests` 只声明 best-effort atomic publish，不宣称 durable directory sync 完成
- restart/load 时会校验：
  - `cluster_id` mismatch -> fail-fast
  - `node_type` mismatch -> fail-fast
  - `node_id` mismatch -> fail-fast
  - Metadata `raft_id` mismatch -> fail-fast
  - `membership_state` mismatch -> fail-fast
  - `source` mismatch -> fail-fast
- old-format / corrupt / missing required fields（如缺 `membership_state`、`persistent_generation`）-> fail-fast。
- 创建失败或 mismatch 失败时不会自动覆盖已有 `node.identity`。
- staging 文件写失败、flush 失败或 publish 失败时会清理 staging 文件；旧 final file 不会被当作成功重写。
- create-only 冲突时若是并发首启，代码会 retry load 一次，把“别人先写成功”的正常复用识别为 loaded_existing，而不是误报失败。

## Platform Durability Notes

### Linux

- Linux 是当前主验证平台。
- 已有前置任务报告确认：
  - `test_node_identity` targeted build 多轮通过
  - `NodeIdentityTest.*` targeted CTest 多轮通过
  - T016 还完成了清理旧运行数据后的 local RPC `status` smoke，通过了 ViewNode / MetadataNode / StorageNode 的 identity wiring 启动路径
- Linux 已验证的是逻辑层原子 publish / restart validation 边界，包括 `fsync(fd) -> atomic publish -> fsync(data_dir)` 的代码路径和测试覆盖。
- 当前没有做真实断电 / power-loss / crash-at-arbitrary-instruction 级别的 durability 实机验证，因此不能把 Linux 写成“断电级 durability 已完全证明”。

### Windows

- Windows：pending / not run。
- 当前代码有 Windows 路径，使用 `FlushFileBuffers(handle)` + `MoveFileExW(...)`。
- 但 required durability 下，Windows 目录 durability 仍未实机验证，也未在代码里宣称完成；当前实现选择显式返回错误，拒绝 silent success。
- 后续仍需要 Windows targeted validation，不能写成 PASS。

### macOS

- macOS：pending / not run。
- 009 当前不是以 macOS 为主验证平台。
- 如后续要宣称 macOS durability 行为，需要单独做 targeted validation；当前不能写成 PASS。

## Validation Performed

本任务实际执行的是文档与存在性收口，不重新跑 build/test。

- 文件存在性检查：PASS
  - `modules/cluster/node_identity.h`
  - `modules/cluster/node_identity.cpp`
  - `tests/node_identity_test.cpp`
  - `apps/storage_node_app.cpp`
  - `apps/view_node_app.cpp`
  - `apps/metadata_node_app.cpp`
- legacy / auto-upgrade 关键词 grep：已执行
  - 结果：仅保留否定语义或历史报告痕迹
  - 发现 `t011-extend-...` 历史报告仍保留被 `t011-fix-...` 推翻的旧兼容结论；本任务不改写历史报告正文，只在本收口报告中显式以新语义覆盖
- build：Not run
- test：Not run
- 原因：本任务是 documentation-only closure，依赖 T006-T016 已完成的 targeted build/test 与 T016 local RPC smoke 结果，不重复触发无必要构建。

## Build Lock

- Not required for this documentation-only closure task.

## Boundary Checks

- 没有修改生产代码。
- 没有修改测试断言。
- 没有修改 proto / 协议语义。
- 没有修改 CMake。
- 没有实现 ViewNode self refresh。
- 没有实现 StorageNode dynamic join。
- 没有实现 Metadata learner join。
- 没有修改 Raft membership authority。
- 没有恢复 legacy v1 compatibility。

## Remaining Follow-ups

- T018 起进入 ViewNode self refresh，不在 Phase 2 中实现。
- ViewNode peer sync 在 Phase 5。
- StorageNode dynamic join 在 Phase 6。
- Metadata learner join 在 Phase 7。
- learner catch-up / odd voter / batch promote 在 Phase 8-9。
- Windows/macOS durability validation 继续 pending。
- identity atomic publish 当前只有逻辑层验证，没有 power-loss 级实证；该风险已同步到 `cross-task-risk-notes.md` 的 `R9`。
- 旧的 `t011-extend-...` 历史报告与当前 new-only 语义冲突，但该历史文件不在本任务允许的“明显命名/验证矩阵同步”范围内，故本任务只在收口报告中标注 superseded，不重写历史记录。

## Result

- 最终状态：`PASS`
- Phase 2 identity lifecycle 收口文档已完成。
- 当前最终语义已经统一为 new-only identity format + fail-fast + atomic publish + process incarnation + app wiring boundary。
- 可以进入 `T018`。
