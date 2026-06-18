# Contract: Local RPC Validation

## Scope

本合同定义 009 阶段本地 RPC example、CTest target、CTest label 与日志输出要求。验证必须基于报告确认的真实入口，不使用凭空假设的脚本或 target。

## Report-Confirmed Existing Entrypoints

- Example: `examples/object-storage-local-3meta-6store`
- Startup: `examples/object-storage-local-3meta-6store/qidong.sh`
- Shutdown: `examples/object-storage-local-3meta-6store/tingzhi.sh`
- Status / roundtrip: `examples/object-storage-local-3meta-6store/rpc_demo.sh status|roundtrip`
- Test files: `tests/test_file`
- App targets: `view_node_app`、`metadata_node_app`、`storage_node_app`、`storage_client`、`raft_metadata_client`
- Existing validated path: `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp`

## CTest Entrypoints

- `test_integrated_object_storage_e2e` / custom target `integrated_object_storage_e2e`
- `test_integrated_object_storage_quorum` / custom target `integrated_object_storage_quorum`
- `test_integrated_object_storage_recovery` / custom target `integrated_object_storage_recovery`
- `test_integrated_object_storage_concurrency` / custom target `integrated_object_storage_concurrency`
- `test_view_node_discovery`
- `test_node_identity`
- `cluster_config_test`
- `storage_heartbeat_registry`

Relevant labels include:

- `integrated-object-storage`
- `integrated-object-storage-e2e`
- `integrated-object-storage-quorum`
- `integrated-object-storage-recovery`
- `integrated-object-storage-concurrency`
- `view-node`
- `node-identity`
- `storage-node`
- `platform-neutral`
- `linux-primary-diagnosis`

## 009 Required Scenarios

- 2 ViewNodes + 3 initial Metadata voters + multiple StorageNodes startup.
- ViewNode self refresh exceeds TTL and remains LIVE.
- ViewNode peer sync spreads metadata/storage observed state.
- Stop one ViewNode and complete discovery through the survivor.
- Run-time StorageNode join becomes visible to future placement and upload/download.
- Run-time Metadata learner join remains non-voter and catches up.
- 3 voters + 1 learner does not promote to 4 voters.
- 3 voters + 2 ready learners can promote to 5 voters only through safe batch membership change.
- identity_file first creation and restart reuse.
- repeated registration, stale heartbeat, old incarnation, stale snapshot, corrupt identity, mismatch identity.

## Command Guidance

- Do not default to full build.
- Prefer targeted build:

```bash
cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e integrated_object_storage_quorum test_view_node_discovery test_node_identity
```

- Prefer targeted CTest by test name or label after CMake wiring exists.
- In concurrent windows, use a build lock. If the lock cannot be acquired, record skipped build/test in `specs/009-local-rpc-object-storage-stabilization/task-reports/`.
- Logs must be saved locally, for example under `tmp/test-logs/` or feature task-report paths. Chat/task reports summarize PASS or failure tail only.

