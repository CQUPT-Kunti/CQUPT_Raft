# T031 - metadata transfer client 接口定义

## 1. 修改了哪些文件

- `modules/store/transfer/metadata_transfer_client.h`
  - 新增 MetadataService 适配层接口头文件，只定义 transfer -> metadata adapter 的类型、接口和职责边界。
- `modules/store/transfer/module-notes.md`
  - 最小补充 `metadata_transfer_client.h` 的接口边界说明。
- `specs/008-integrated-object-storage-system/tasks.md`
  - 仅将 `T031` 从 `[ ]` 更新为 `[X]`。

本任务未修改：

- `proto/`
- `tests/`
- `apps/`
- `common-risk-notes.md`
- `risk-register.md`

## 2. `metadata_transfer_client.h` 定义了什么接口边界

当前头文件定义了以下边界：

1. 状态与诊断类型
- `MetadataTransferStatusCode`
  - 覆盖 `NOT_LEADER`、`idempotent replay`、`idempotency conflict`、`state conflict`、`object not visible`、`quorum unavailable`、`timeout`、`service unavailable` 等 transfer 关心的 metadata 结果边界。
- `MetadataTransferLeaderHint`
- `MetadataTransferSummary`
- `MetadataTransferDiagnostic`
- `MetadataTransferClientCallDiagnostics`
- `MetadataTransferClientCallResult<T>`

2. 调用配置
- `MetadataTransferClientConfig`
  - 为 `CreateWritePlan`、`CommitObject`、`HeadObject`、`GetObjectManifest` 预留默认 timeout 和 `wait_for_ready` 配置。
- `MetadataTransferClientCallOptions`
  - 允许单次调用覆盖 timeout / `wait_for_ready`。

3. transfer 请求/结果模型
- `MetadataTransferCreateWritePlanRequest/Result`
- `MetadataTransferCommitObjectRequest/Result`
- `MetadataTransferHeadObjectRequest/Result`
- `MetadataTransferGetObjectManifestRequest/Result`
- `TransferObjectHead`

这些类型明确了：

- `request_id`
- bucket / object_key / object_id / version
- object checksum / etag / chunk manifest facts
- `leader_hint`
- 幂等 replay / 冲突 / 可见性 / transport 诊断边界

4. adapter 类声明
- `MetadataTransferClient`
  - 构造边界：可从 `raft::MetadataService::StubInterface` 或 `grpc::Channel` 构造
  - 方法边界：
    - `CreateWritePlan`
    - `CommitObject`
    - `HeadObject`
    - `GetObjectManifest`
  - 查询边界：
    - `target_endpoint()`
    - `config()`

## 3. 是否保持只定义接口、不实现 MetadataService 调用逻辑

已保持。

本任务只在 `.h` 中定义了：

- 请求/结果结构
- 调用配置
- 诊断结构
- `MetadataTransferClient` 类声明

本任务没有实现：

- 任何 MetadataService RPC 调用逻辑
- proto 请求构造
- proto 响应转换
- leader retry
- ViewNode discovery
- upload/download 编排
- StorageNode chunk 读写

这些仍然留给后续 `T032` 和更后面的集成任务处理。

## 4. 是否发现不合理点 / 警告 / 风险

有两点需要记录：

- 当前 `metadata.proto` 现有 RPC 名称仍是 `CreateObject` / `CommitObject` / `HeadObject`，并没有直接名为 `CreateWritePlan` / `GetObjectManifest` 的 RPC。因此本次接口把 `CreateWritePlan` / `GetObjectManifest` 定义为 transfer 侧“逻辑边界”，后续 `T032` 需要在 adapter 实现里完成逻辑接口到现有 MetadataService RPC 的映射。
- 当前工作区 diff 中存在本任务之外的外部变更痕迹：
  - `tasks.md` 中可见 `T029`、`T033` 也已经被勾选
  - `module-notes.md` 中也存在与 `T029/T033` 对应的补充内容
  - 本任务没有修改或验证这些外部变更，只新增了 `T031` 所需接口与最小说明

## 5. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

未修改 `common-risk-notes.md`，未修改 `risk-register.md`。

## 6. 验证命令和结果

已执行：

1. diff 检查

```bash
git diff -- modules/store/transfer/metadata_transfer_client.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md
git status --short -- modules/store/transfer/metadata_transfer_client.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t031-metadata-transfer-client-interface.md
```

结果：

- `metadata_transfer_client.h` 已新增
- `module-notes.md` 已同步最小边界说明
- `tasks.md` 已将 `T031` 勾选为完成
- diff 中同时可见工作区内已有的 `T029/T033` 相关外部差异，这些不是本任务新增

2. 最小相关 target 构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core' || echo "build lock busy, skip build in this window"
```

结果：

- 成功拿到构建锁
- `cmake --preset debug-ninja-safe` 配置成功
- `cmake --build --preset debug-ninja-safe --target raft_core` 成功
- 输出为 `ninja: no work to do.`

## 结论

- `T031` 已完成。
- 当前实现保持在“只定义接口、不实现调用逻辑”的边界内。
- 可以进入 `T032`，由后续任务实现 MetadataService 调用适配逻辑。
