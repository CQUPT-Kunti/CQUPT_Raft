# Quickstart: 009 Local RPC Object Storage Stabilization

## Purpose

本 quickstart 记录 009 阶段建议的最小验证路径。它不是执行流水；真实执行结果写入 `specs/009-local-rpc-object-storage-stabilization/task-reports/`。

## Baseline Inputs

- Report: `specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`
- Example: `examples/object-storage-local-3meta-6store`
- Scripts: `qidong.sh`、`tingzhi.sh`、`rpc_demo.sh status|roundtrip`
- App targets: `view_node_app`、`metadata_node_app`、`storage_node_app`、`storage_client`、`raft_metadata_client`
- Test data: `tests/test_file`

## Targeted Build

不要默认全量构建。优先构建当前任务相关 target：

```bash
cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e integrated_object_storage_quorum test_view_node_discovery test_node_identity storage_heartbeat_registry
```

并发窗口下使用构建锁。抢不到锁时跳过并在 task report 说明。

## Baseline Local RPC Check

```bash
examples/object-storage-local-3meta-6store/qidong.sh
examples/object-storage-local-3meta-6store/rpc_demo.sh status
examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip
examples/object-storage-local-3meta-6store/tingzhi.sh
```

期望：

- Metadata/RaftNode 3 voters 稳定选出一个 leader。
- `CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp` 成功。
- 008 静态 6 StorageNode 路径保持可用。
- 当前 009 未修复前，ViewNode self-liveness 可能暴露 stale/dead 问题；修复后应保持 LIVE。

## 009 Scenario Checks

### ViewNode Self Refresh

- 入口：`tests/view_node_discovery_test.cpp`
- 验证：单 ViewNode 运行超过 dead TTL 后自身仍 `LIVE`。
- 负向验证：停止 self refresh 后按 TTL 转为 `STALE`、`SUSPECT`、`DEAD`。

### ViewNode Peer Sync

- 入口：`tests/view_node_discovery_test.cpp` 或新增 ViewNode peer sync 测试。
- 验证：两个 ViewNode 同步 observed registry；旧 incarnation / stale snapshot 不能覆盖新 incarnation。

### StorageNode Dynamic Join

- 入口：`tests/storage_heartbeat_registry_test.cpp`、`tests/integrated_object_storage_e2e_test.cpp`、local RPC example。
- 验证：运行中新增 StorageNode，后续新对象 placement 可以使用它，旧对象不要求 rebalance。

### Metadata Learner Join

- 入口：`tests/integrated_object_storage_quorum_test.cpp`、`tests/test_raft_log_replication.cpp`、`tests/test_raft_snapshot_catchup.cpp`、`tests/test_raft_election.cpp`。
- 验证：3 voters + 1 learner quorum 仍为 2，learner 可追日志/snapshot，不能投票/当 leader，不能 promote 成 4 voters。

### Batch Promote

- 入口：`tests/integrated_object_storage_quorum_test.cpp` 或新增 membership batch 测试。
- 验证：3 voters + 2 ready learners 可安全变成 5 voters，quorum 从 2 变 3，中间没有 committed 4 voters。

## Log Rules

- 测试通过只记录命令、PASS、总耗时。
- 测试失败只记录失败测试名、关键断言、失败分类、最后 50 行日志、完整日志路径。
- 不把完整 Raft 节点日志贴到聊天或高频 spec/plan/tasks 文档中。

