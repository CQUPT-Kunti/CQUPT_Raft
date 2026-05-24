# Quickstart: Metadata-Only 主路径验证

## 当前状态

- 当前主路径已经切到 metadata-only：
  - 默认业务状态机是 `MetadataStateMachine`
  - 默认业务服务是 `MetadataService`
  - 默认业务 CLI 是 `raft_metadata_client`
  - retired KV service/client/proto/doc surface 已纳入 `NoKvSurfaceAudit`
- 当前**不能**宣称“KV 物理删除完成”：
  - `CommandType::kSet/kDelete`
  - `KvStateMachine`
  - `modules/raft/state_machine/state_machine.h/.cpp`
  - `tests/test_state_machine.cpp`
  - 仍是已知 blocker
- 当前任务状态中，Linux 与 Windows 最终验证均已通过。
- 历史 task report 中仍保留过往阶段性失败/补测记录，应视为过程快照，不代表当前最终状态。

## 1. Linux Configure / Build

推荐先跑主路径与审计相关 target：

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target \
  raft_demo \
  raft_metadata_client \
  no_kv_surface_audit
```

如需补建 metadata 主路径测试：

```bash
cmake --build --preset debug-ninja-low-parallel --target \
  test_metadata_state_machine \
  test_metadata_client_scenario \
  test_metadata_failover \
  test_metadata_snapshot \
  test_metadata_recovery_stress
```

## 2. Linux Metadata-Focused CTest

先跑当前 metadata-only 主路径的基础验证：

```bash
ctest --test-dir build/linux --output-on-failure -R \
  '^(Metadata(Command|StateMachine|Snapshot|Failover|ClientScenario|Manifest)Test)\.'
```

如需补跑 metadata recovery stress：

```bash
ctest --test-dir build/linux --output-on-failure -R \
  '^MetadataRecoveryStressTest\.'
```

说明：

- 这些命令用于验证 metadata-only 主路径是否可用。
- 它们是日常回归的最小入口；如需做最终验收，请继续执行下文的 recovery/full-validation 集合。

## 3. no-KV Surface Audit

轻量 no-KV 审计请直接使用以下命令：

```bash
cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit
ctest --test-dir build/linux --output-on-failure -R 'NoKvSurfaceAudit'
```

当前不建议把下面命令当作“轻量 no-kv 审计”：

```bash
./test.sh --skip-configure --skip-build --group no-kv
```

原因：

- 当前脚本入口会扩展到更大的 CTest 集合，而不只是 `NoKvSurfaceAudit`
- 它更适合作为脚本级总入口，而不是单独的轻量审计命令

## 4. Manual Cluster Smoke

启动 `raft_demo` 后，用 `raft_metadata_client` 验证 metadata-only 业务流。

启动节点示例：

```bash
./build/linux/raft_demo config.txt 1
```

metadata-only CLI 示例：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 create-bucket --request-id b1 --bucket bucket-a
./build/linux/raft_metadata_client 127.0.0.1:50051 create-object --request-id c1 --bucket bucket-a --object object/a --object-size 1024 --chunk-size 256 --chunk-count 4 --checksum checksum-a --mock-location node-a
./build/linux/raft_metadata_client 127.0.0.1:50051 commit-object --request-id m1 --bucket bucket-a --object object/a --expected-create-request-id c1
./build/linux/raft_metadata_client 127.0.0.1:50051 head-object --bucket bucket-a --object object/a
./build/linux/raft_metadata_client 127.0.0.1:50051 list-objects --bucket bucket-a
./build/linux/raft_metadata_client 127.0.0.1:50051 delete-object --request-id d1 --bucket bucket-a --object object/a
```

## 5. Snapshot / Restart / Catch-up / Recovery 入口

扩展验证入口：

```bash
cmake --build --preset debug-ninja-low-parallel --target \
  test_metadata_snapshot \
  test_metadata_state_machine \
  test_metadata_recovery_stress \
  snapshot_test \
  persistence_test \
  test_raft_snapshot_catchup \
  test_raft_snapshot_restart \
  test_raft_snapshot_diagnosis \
  test_raft_split_brain \
  test_raft_replicator_behavior \
  test_raft_segment_storage
```

```bash
ctest --test-dir build/linux --output-on-failure -R \
  '(MetadataSnapshotTest|MetadataStateMachineTest|MetadataRecoveryStressTest|RaftSnapshotRecoveryTest|PersistenceTest|RaftSnapshotCatchupTest|RaftSnapshotRestartTest|RaftSnapshotDiagnosis|RaftSplitBrain|RaftReplicatorBehavior|RaftSegmentStorage|SnapshotStorageReliability)'
```

当前状态说明：

- 这组命令用于 recovery/snapshot/catch-up/final validation 主路径
- 当前任务状态中，这些 Linux 验证已完成通过
- 历史 `T043/T051` 报告保留的是修复前或中间轮次记录，可作为过程追溯使用

## 6. Windows 入口

Windows 可参考以下命令做主路径补测：

```powershell
cmake --preset windows
cmake --build --preset windows-debug --target raft_demo raft_metadata_client no_kv_surface_audit
ctest --test-dir build/windows -C Debug --output-on-failure -R "^(MetadataStateMachineTest|MetadataFailoverTest|MetadataClientScenarioTest)\."
```

当前状态说明：

- 当前任务状态中，Windows configure/build/CTest 最终验证已通过
- 这里保留的是 Windows 复验入口，便于后续手工补测或复跑
- 历史 `t027-windows-validation.md` 属于早期阶段性补测记录

## 7. 验收口径

当前可以认为：

- metadata-only 主路径已建立
- retired KV service/client/proto/doc surface 已从主构建与主测试入口退出
- `NoKvSurfaceAudit` 已接入
- Linux 全量最终验证已通过
- Windows 全量最终验证已通过

当前不能认为：

- KV 物理删除已完成
