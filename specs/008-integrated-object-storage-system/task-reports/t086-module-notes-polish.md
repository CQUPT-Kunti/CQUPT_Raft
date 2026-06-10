# T086 - module notes polish

## 1. 修改了哪些文件

- `modules/cluster/module-notes.md`
- `modules/view/module-notes.md`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t086-module-notes-polish.md`

本任务没有修改：

- 生产代码
- 测试代码
- `tests/CMakeLists.txt`
- `proto/`
- `quickstart.md`

## 2. modules/cluster/module-notes.md 更新了什么

- 从“规划中职责”改成了“当前代码已实现职责”表述。
- 按实际接口补齐了：
  - `ValidateClusterConfig(...)`
  - `AllocateClusterEndpoints(...)`
  - `GenerateDeterministicClusterConfig(...)`
  - `ResolveClusterNodeConfig(...)`
  - `ComputeInitialRaftQuorum(...)`
  - `LoadClusterConfigFromJsonFile(...)`
- 按当前 `node_identity.h/.cpp` 补齐了：
  - `NodeIdentity`
  - `ExpectedNodeIdentity`
  - `LoadNodeIdentity(...)`
  - `StoreNodeIdentity(...)`
  - `LoadOrCreateNodeIdentity(...)`
  - identity/config mismatch 与 raft_id mismatch 诊断边界
- 明确写出了 Linux/Windows durability contract：
  - Linux 的临时文件写入、flush、publish、目录 `fsync`
  - Windows 的 `FlushFileBuffers`、`MoveFileExW`、`kRequired` / `kBestEffortForTests`
- 明确 cluster 模块不是：
  - runtime membership authority
  - ViewNode discovery 实现
  - StorageNode chunk durability/recovery 实现

## 3. modules/view/module-notes.md 更新了什么

- 改为按当前代码说明：
  - `ViewNodeRegistry`
  - `ViewNodeServiceImpl`
  - `ViewNodeClient`
- 补齐了 registry 当前真实职责：
  - 注册校验
  - endpoint / `node_id` / `data_dir_fingerprint` 冲突诊断
  - heartbeat sequence 去旧
  - liveness 推导
  - Metadata membership 观测状态保守归一化
  - discovery / cluster view 生成
- 补齐了 service adapter 当前真实职责：
  - proto <-> 本地类型映射
  - `now_unix_ms` 注入
  - StorageNode first registration 的 `node_id` 分配 / 确认路径
- 明确写出 StorageNode first registration 的使用边界和误用风险：
  - 只适用于 StorageNode
  - 不分配 MetadataNode `raft_id`
  - 不改写本地 `node.identity`
- 再次强调 ViewNode 不是：
  - metadata authority
  - membership authority
  - payload path

## 4. modules/store/transfer/module-notes.md 更新了什么

- 从“规划说明”改成了“当前实现说明”。
- 按实际代码补齐了 upload/download 主流程：
  - upload 的“两遍 bounded 读取 + `CreateWritePlan` + `WriteChunk` + `CommitObject`”
  - download 的 “COMMITTED manifest -> `ReadChunk` -> 临时文件重建 -> 最终 checksum”
- 补齐了三个适配层的当前语义：
  - `ObjectTransfer`
  - `MetadataTransferClient`
  - `StorageTransferClient`
- 明确写出 metadata adapter 的现状：
  - `CreateWritePlan` 当前最小映射到现有 `CreateObject`
  - `GetObjectManifest` 当前通过 `HeadObject` 取 COMMITTED 对象
  - `NOT_LEADER` 只做一次 leader hint endpoint 刷新重试
- 明确写出 storage adapter 的现状：
  - 单节点 `WriteChunk` / `ReadChunk`
  - transient retry/backoff 只针对临时失败
  - 不把 checksum/data corruption 伪装成可恢复重试
- 按 T081/T082/T083 补齐了：
  - failed upload cleanup candidate emission
  - retry/backoff 诊断边界
  - session 级 bounded concurrency budget
  - 当前 `effective_concurrency=1` 的实际状态
- 重申真实 payload 只能走 StorageNode data-plane，不能进 Raft log/snapshot/metadata snapshot/task report。

## 5. 是否发现不合理点 / 警告 / 风险

- `tasks.md` 当前工作树里已经存在与本任务无关的 T084/T085/T088 状态差异；本任务只额外把 T086 从 `[ ]` 改为 `[X]`，没有处理后续任务内容。
- `modules/store/transfer/module-notes.md` 之前存在较多“当前规划中/后续负责”的残留表述，和已经完成的 T081/T082/T083/T084 不完全同步；本任务已收口成“以当前代码为准”的维护说明。
- transfer 当前虽然已经有 bounded concurrency 预算，但仍不是“真正多 chunk 并发 pipeline”；文档里已明确写成当前限制，而没有夸大成已完成高压并发能力。

## 6. 是否修改 risk-register.md

- 未修改 `risk-register.md`

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- modules/cluster/module-notes.md modules/view/module-notes.md modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t086-module-notes-polish.md
```

结果：已执行。三个 `module-notes.md`、`tasks.md` 和本任务报告存在改动。

说明：

- `tasks.md` 工作树里还包含之前任务留下的 T084/T085/T088 勾选差异；本任务只新增了 `T086=[X]`。

### 代码对照检查

已对照以下当前实现做文档准确性核对：

- `modules/cluster/cluster_config.h`
- `modules/cluster/node_identity.h`
- `modules/cluster/cluster_config.cpp`
- `modules/cluster/node_identity.cpp`
- `modules/view/view_registry.h`
- `modules/view/view_service_impl.h`
- `modules/view/view_client.h`
- `modules/view/view_registry.cpp`
- `modules/view/view_service_impl.cpp`
- `modules/view/view_client.cpp`
- `modules/store/transfer/object_transfer.h`
- `modules/store/transfer/metadata_transfer_client.h`
- `modules/store/transfer/storage_transfer_client.h`
- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/metadata_transfer_client.cpp`
- `modules/store/transfer/storage_transfer_client.cpp`

### 构建 / smoke

- 本任务是文档收口任务，默认不需要 `cmake configure/build/ctest`。
- 因此本任务未运行构建或 smoke build。
