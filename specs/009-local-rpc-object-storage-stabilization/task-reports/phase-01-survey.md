# T001 Phase 1 Survey

## Scope

本任务只做 Phase 1 文档勘察与基线路径收口，不写业务代码、不改协议、不改测试逻辑、不执行构建或测试变更。

## Report Source

本次记录的主依据为：

- `specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`

本次交叉对照的 009 文档为：

- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/local-rpc-validation.md`

## Confirmed Local RPC Baseline

- 当前 local RPC example 路径：`examples/object-storage-local-3meta-6store`
- 当前真实 RPC topology：
  - `ViewNode=1`
  - `MetadataNode=3`
  - `StorageNode=6`
- 当前 client 使用方式：客户端通过 `storage_client` 走真实 gRPC / RPC
- 当前测试文件目录：`tests/test_file`
- 当前基线目标：保持 008 已跑通的静态本地 RPC roundtrip，不把 ViewNode 变成 Raft membership authority

## Confirmed Scripts

- 启动脚本：`examples/object-storage-local-3meta-6store/qidong.sh`
- 停止脚本：`examples/object-storage-local-3meta-6store/tingzhi.sh`
- 状态检查脚本：`examples/object-storage-local-3meta-6store/rpc_demo.sh status`
- roundtrip 脚本：`examples/object-storage-local-3meta-6store/rpc_demo.sh roundtrip`

补充确认：

- `rpc_demo.sh` 的已记录入口包含 `status|upload|download|roundtrip`
- `qidong.sh` 启动 `view-1`、`meta-1..meta-3`、`store-1..store-6`
- `tingzhi.sh` 按逆序停止上述节点

## Confirmed App Targets

当前报告和 009 文档确认的 app targets：

- `view_node_app`
- `metadata_node_app`
- `storage_node_app`
- `storage_client`
- `raft_metadata_client`

## Confirmed Validated Path

当前真实 RPC 已验证调用链为：

`CreateWritePlan -> WriteChunk -> CommitObject -> Download -> cmp`

补充确认：

- roundtrip 会从 `tests/test_file` 收集真实文件
- 逐个 upload
- 逐个 download
- 最后做本地 `cmp` 比对

## Confirmed Existing Fixes From Report

report 已确认的 008 收口修复及关键文件：

1. Metadata leader 抖动修复
   - 文件：`apps/metadata_node_app.cpp`
2. bucket 缺失前置拒绝
   - 文件：`modules/raft/service/metadata_service_impl.cpp`
3. `CreateWritePlan` 版本分配修复
   - 文件：`modules/raft/service/metadata_service_impl.cpp`
4. `CommitObject` request_id 分离
   - 文件：`modules/store/transfer/object_transfer.cpp`

已记录的修复结果包括：

- 3 个 MetadataNode 可稳定形成单 leader
- bucket 缺失不再进入 committed 无效 apply 路径
- `CreateWritePlan` 返回可用正数版本
- `CommitObject` 不再因复用 request_id 触发幂等冲突

## Confirmed Known Gaps

当前 report 与 009 规划共同确认的已知缺口：

- ViewNode self-liveness 仍可能显示为 `stale/dead`
  - 原因：ViewNode 自身 registry 记录没有持续 self-heartbeat / self refresh
- 当前 discovery 基线仍是单 ViewNode，尚未进入 009 目标中的多 ViewNode active-active peer sync
- 当前 local RPC 基线仍是静态 topology，不等于已经完成 StorageNode dynamic join、ViewNode peer sync、Metadata learner join

## Confirmed CTest / Validation Entrypoints

后续 009 任务至少会依赖以下 CTest test name / custom target：

- `test_view_node_discovery`
- `test_node_identity`
- `storage_heartbeat_registry`
- `test_integrated_object_storage_e2e` / custom target `integrated_object_storage_e2e`
- `test_integrated_object_storage_quorum` / custom target `integrated_object_storage_quorum`

本任务同时确认到的相关扩展入口：

- `test_integrated_object_storage_recovery` / custom target `integrated_object_storage_recovery`
- `test_integrated_object_storage_concurrency` / custom target `integrated_object_storage_concurrency`
- `cluster_config_test`

后续 009 文档已明确的相关 label 包括：

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

## Validation Principles Confirmed

后续验证应遵守以下原则：

- 不默认全量构建
- 优先 targeted build / targeted CTest
- 构建命令优先使用项目已有 CMake / CTest 流程
- 并发窗口下使用 `flock` 构建锁
- 构建锁抢不到时可以跳过 build/test，并在 task report 中说明
- Windows 无实机时标记待测，不伪造通过
- 日志保存在本地路径，聊天或 task report 只写 PASS 摘要或失败摘要

本任务从合同与计划中确认到的 targeted build 基线示例：

```bash
cmake --build --preset debug-ninja-low-parallel --target view_node_app metadata_node_app storage_node_app storage_client raft_metadata_client
cmake --build --preset debug-ninja-low-parallel --target integrated_object_storage_e2e integrated_object_storage_quorum test_view_node_discovery test_node_identity
```

## Validation

本任务完成的验证：

- 已阅读 `local-rpc-object-storage-stabilization-report.md`
- 已对照 `tasks.md`、`plan.md`、`validation-matrix.md`、`contracts/local-rpc-validation.md`
- 已执行轻量文件存在性检查：
  - `test -f specs/009-local-rpc-object-storage-stabilization/task-reports/local-rpc-object-storage-stabilization-report.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/tasks.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/plan.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
  - `test -f specs/009-local-rpc-object-storage-stabilization/contracts/local-rpc-validation.md`
- 未执行 build/test
  - 原因：T001 是文档勘察任务，只需要文本确认和文件存在性检查，不需要进入构建或运行验证

## Next Step

T001 已完成当前基线路径收口，可以进入 `T002`，继续确认 `examples/object-storage-local-3meta-6store` 及其脚本作为 local RPC baseline 的地位是否需要进一步补充到 `validation-matrix.md`。

