# Validation Matrix: Strong Consistency Metadata Layer

## Scope

本文件记录当前已经实现并有验证记录的 Metadata 能力映射。它只回填稳定的测试目标、Linux 已验证状态、Windows 待验证状态和当前阶段边界，不新增源码、测试或平台执行结果。

本矩阵当前只覆盖 metadata control plane：

- create / commit / head / list / delete
- committed-only visibility
- `request_id` 幂等与 conflict
- tombstone delete
- snapshot / restart recovery
- leader failover
- metadata client scenario

本矩阵不覆盖：

- `StorageNode`
- `ChunkStore`
- 真实 chunk 文件
- 真实上传下载
- repair / rebalance
- S3 协议

## Current Test Target Mapping

| Test Target | CTest Regex / Target | Current Linux Evidence | Current Windows Status | Notes |
|-------------|----------------------|------------------------|------------------------|-------|
| `test_metadata_command` | `^MetadataCommandTest\.` | 已验证，`9/9 PASS` | 待 `T041-T043` | 覆盖 codec、invalid argument、payload 上限、`mock_locations` 解析 |
| `test_metadata_state_machine` | `^MetadataStateMachineTest\.` | 已验证，`6/6 PASS` | 待 `T041-T043` | 覆盖 create/commit 可见性、幂等、缺失 pending commit |
| `test_metadata_snapshot` | `^MetadataSnapshotTest\.` | 已验证，`5/5 PASS` | 待 `T041-T043` | 覆盖 snapshot/restart、tombstone、pending 恢复不可见 |
| `test_metadata_failover` | `^MetadataFailoverTest\.` | 已验证，`2/2 PASS` | 待 `T041-T043` | 覆盖 failover 后 committed 保留、pending 不暴露、同 commit request_id retry |
| `test_metadata_manifest` | `^MetadataManifestTest\.` | 已验证，`7/7 PASS` | 待 `T041-T043` | 覆盖 manifest 边界、payload boundary、`mock_locations` 边界 |
| `test_metadata_client_scenario` | `^MetadataClientScenarioTest\.` | 已验证，`5/5 PASS` | 待 `T041-T043` | 覆盖 metadata client create/commit/head/list/delete/read-after-write/retry 场景 |
| `raft_metadata_client` | build target only | 已在现有 Linux 报告中构建通过，并被 `MetadataClientScenarioTest` 调用 | 待 `T041-T043` | client target 已接入构建链路，当前场景验证基于该 target 完成 |

## Matrix

| ID | Scenario | Primary Test Mapping | Linux Status | Windows Status | Notes |
|----|----------|----------------------|--------------|----------------|-------|
| VM-001 | Create 后 Pending 不可见 | `MetadataStateMachineTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | create 成功后 `Head/List` 不返回对象 |
| VM-002 | Commit 后 Committed 可见 | `MetadataStateMachineTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | commit 成功后 `Head` 返回 record，`List` 包含 `object_key` |
| VM-003 | Duplicate create 幂等 | `MetadataStateMachineTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | 同 `request_id` + 同内容重放稳定 |
| VM-004 | request_id 内容冲突 | `MetadataCommandTest`, `MetadataStateMachineTest` | 已验证 | 待 `T041-T043` | 同 `request_id` + 不同内容返回 `IDEMPOTENCY_CONFLICT` |
| VM-005 | Commit retry 幂等 | `MetadataStateMachineTest`, `MetadataFailoverTest` | 已验证 | 待 `T041-T043` | 同 commit `request_id` 重试不产生重复可见记录 |
| VM-006 | Missing Pending commit | `MetadataStateMachineTest` | 已验证 | 待 `T041-T043` | 缺失可提交 `Pending` 时返回错误且不产生可见记录 |
| VM-007 | Delete tombstone | `MetadataStateMachineTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | delete 后对象不可见，删除事实保留 |
| VM-008 | Delete retry 幂等 | `MetadataSnapshotTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | 同 delete `request_id` 重试稳定 |
| VM-009 | Delete Pending conflict | `MetadataStateMachineTest` | 已验证 | 待 `T041-T043` | delete pending 返回 `STATE_CONFLICT`，对象仍不可见 |
| VM-010 | Deleted 防旧请求复活 | `MetadataSnapshotTest` | 已验证 | 待 `T041-T043` | tombstone 恢复后旧 create/commit 不得复活对象 |
| VM-011 | Snapshot/restart 恢复 committed metadata | `MetadataSnapshotTest` | 已验证 | 待 `T041-T043` | committed records 恢复后仍可见 |
| VM-012 | Snapshot/restart 恢复 tombstone | `MetadataSnapshotTest` | 已验证 | 待 `T041-T043` | deleted object 恢复后仍不可见 |
| VM-013 | Pending restart 不外部可见 | `MetadataSnapshotTest` | 已验证 | 待 `T041-T043` | pending 恢复后仍不对 `Head/List` 可见 |
| VM-014 | Leader failover 保留 committed metadata | `MetadataFailoverTest` | 已验证 | 待 `T041-T043` | 新 leader 上 committed metadata 仍可查询 |
| VM-015 | Leader failover 不暴露 Pending | `MetadataFailoverTest` | 已验证 | 待 `T041-T043` | failover 后 pending 仍不可见 |
| VM-016 | Failover 后 commit retry | `MetadataFailoverTest` | 已验证 | 待 `T041-T043` | 相同 commit `request_id` 可在新 leader 上稳定重试 |
| VM-017 | Simulated manifest validation | `MetadataCommandTest`, `MetadataManifestTest` | 已验证 | 待 `T041-T043` | 合法 manifest 接受，非法 manifest 拒绝 |
| VM-018 | Payload boundary | `MetadataCommandTest`, `MetadataManifestTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | 超过 `4096` 字节 payload 返回 `INVALID_ARGUMENT` |
| VM-019 | List deterministic ordering | 未单独建立专项断言；当前由 `MetadataClientScenarioTest` 间接覆盖 list 基本可见性 | 部分覆盖，待补强 | 待 `T041-T043` | 当前已验证 committed-only list；按 `object_key` 确定性排序尚无独立专项测试记录 |
| VM-020 | StorageNode boundary | `MetadataManifestTest`, `MetadataClientScenarioTest` | 已验证 | 待 `T041-T043` | `mock_locations` 指向不存在节点/伪路径仍可通过 metadata 流程 |

## Linux Validation Evidence

以下结果来自现有任务报告中的已执行验证，不是本次文档任务重新运行：

| Area | Evidence |
|------|----------|
| Metadata command | `T008`: `ctest --test-dir build/linux --output-on-failure -R '^MetadataCommandTest\.'` -> `9/9 PASS` |
| Metadata state machine | `T013`: `ctest --test-dir build/linux --output-on-failure -R '^MetadataStateMachineTest\.'` -> `6/6 PASS` |
| Metadata snapshot | `T021`: `ctest --test-dir build/linux --output-on-failure -R '^MetadataSnapshotTest\.'` -> `5/5 PASS` |
| Metadata failover | `T023`: `ctest --test-dir build/linux --output-on-failure -R '^MetadataFailoverTest\.'` -> `2/2 PASS` |
| Metadata manifest | `T028`: `ctest --test-dir build/linux --output-on-failure -R '^MetadataManifestTest\.'` -> `7/7 PASS` |
| Metadata client scenario | `T034`: `ctest --test-dir build/linux --output-on-failure -R '^MetadataClientScenarioTest\.'` -> `5/5 PASS` |
| Combined metadata suite | `T034`: `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|Manifest|StateMachine|Snapshot|Failover|ClientScenario)Test'` -> `34/34 PASS` |

## Platform Status

### Linux

- 当前矩阵中的 Linux 状态只引用既有任务报告中的已执行验证。
- 本次没有执行 `T038` 的 Linux configure/build validation。
- 当前能明确标记为 Linux 已验证的范围：
  - `MetadataCommandTest`
  - `MetadataManifestTest`
  - `MetadataStateMachineTest`
  - `MetadataSnapshotTest`
  - `MetadataFailoverTest`
  - `MetadataClientScenarioTest`

### Windows

- 当前没有新的 Windows 执行结果可回填。
- 所有 Windows 平台状态统一标记为待 `T041-T043` 验证。
- 不将 Linux 通过结果外推为 Windows 已通过。

## Current Gaps

- `VM-019` 的 “按 `object_key` 确定性排序” 目前没有独立专项测试记录，当前只可追踪到 list 基本可见性与 committed-only 语义。
- 当前阶段没有为真实 `StorageNode`、真实 chunk、S3、rebalance、repair 增加验证项，这些内容不属于本 feature 当前范围。

## Out Of Scope Validation

- 不验证真实文件上传下载。
- 不验证真实 chunk 落盘。
- 不验证 `StorageNode` 可达性。
- 不验证 chunk replication、纠删码、rebalance、repair、S3 协议。
- 不通过读取 Raft 内部日志、snapshot 产物或禁止路径作为验收手段。
