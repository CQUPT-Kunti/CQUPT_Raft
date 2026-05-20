# Quickstart: Validate Metadata-Only Migration

## 1. Configure and Build

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

更保守：

```bash
cmake --preset debug-ninja-safe
cmake --build --preset debug-ninja-safe
```

## 2. Run Metadata-Focused Unit Coverage

实现完成后，第一批回归应至少覆盖：

```bash
ctest --test-dir build/linux --output-on-failure -R '^(Metadata(Command|StateMachine|Snapshot|Service|ClientScenario)|MetadataManifest)Test\.'
```

目标：

- metadata command codec
- bucket/object 生命周期
- request_id 幂等
- snapshot save/load/replay
- metadata client scenario

## 3. Run Migrated Raft Core Regression

```bash
ctest --test-dir build/linux --output-on-failure -R '^(Raft(LogReplication|CommitApply|Integration|SnapshotCatchup|SnapshotRestart|LeaderSwitchOrdering|SplitBrain)|PersistenceTest)\.'
```

验收重点：

- 不再依赖 `SET/DEL`
- 不再依赖 `DebugGetValue()`
- 断言改为 metadata query 或 metadata state verification

## 4. Run Linux Primary Recovery / Durability Checks

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence
CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-recovery --keep-data
CTEST_PARALLEL_LEVEL=1 ./test.sh --group diagnosis --keep-data
```

目标：

- metadata snapshot publish
- restart recovery after concurrent writes
- catch-up after snapshot or replay
- explicit failure diagnostics

## 5. Validate Windows Fallback Entry

```powershell
.\test.ps1 -Managed
```

预期变化：

- conservative fallback 不再宣传 `KvStateMachineTest`
- managed/full CTest sweep 不再依赖 `test_kv_service`

## 6. Manual Cluster Smoke

启动节点：

```bash
./build/linux/raft_demo config.txt 1
```

执行 metadata-only 操作：

```bash
./build/linux/raft_metadata_client 127.0.0.1:50051 create-bucket --request-id b1 --bucket bucket-a
./build/linux/raft_metadata_client 127.0.0.1:50051 create-object --request-id c1 --bucket bucket-a --object object/a --object-size 1024 --chunk-size 256 --chunk-count 4 --checksum checksum-a --mock-location node-a
./build/linux/raft_metadata_client 127.0.0.1:50051 commit-object --request-id m1 --bucket bucket-a --object object/a --expected-create-request-id c1
./build/linux/raft_metadata_client 127.0.0.1:50051 head-object --bucket bucket-a --object object/a
./build/linux/raft_metadata_client 127.0.0.1:50051 list-objects --bucket bucket-a
./build/linux/raft_metadata_client 127.0.0.1:50051 delete-object --request-id d1 --bucket bucket-a --object object/a
```

## 7. Final Acceptance Checklist

- `raft_kv_client` target 不再构建
- `KvService` / `KvStateMachine` / KV `SET/DEL` 不再属于主路径
- `MetadataService` / `MetadataStateMachine` 成为唯一业务路径
- CTest 与脚本入口不再以 KV 为 fallback
- snapshot + replay + catch-up + leader switch 都通过 metadata-only 路径验证
