# T034 - Storage transfer client 实现

## 1. 修改了哪些文件

- `modules/store/transfer/storage_transfer_client.cpp`
  - 新增 StorageNode chunk read/write adapter 实现。
- `modules/store/transfer/storage_transfer_client.h`
  - 做了最小必要补充：增加 `StorageTransferClientConfig` 和 `CreateGrpcStorageTransferClient(...)` factory 声明，便于 `T034` 落地 concrete adapter。
- `modules/store/transfer/module-notes.md`
  - 最小补充 `T034` 的实现边界说明。
- `specs/008-integrated-object-storage-system/tasks.md`
  - 仅将 `T034` 从 `[ ]` 更新为 `[X]`。

本任务未修改：

- `proto/`
- `tests/`
- `apps/`
- `common-risk-notes.md`
- `risk-register.md`

## 2. StorageNode chunk read/write adapter 做了什么

当前 `storage_transfer_client.cpp` 实现了一个基于 gRPC data-plane 的 transfer adapter，核心行为如下：

1. `CreateGrpcStorageTransferClient(...)`
- 返回 `StorageTransferClient` 的 concrete 实现
- 对外仍保留抽象接口边界，避免把 `object_transfer` 绑死到具体 RPC 细节

2. `WriteChunk`
- 校验 `request_id`、target endpoint、`expected_size` 与 payload size 的基础一致性
- 规范化 `ChunkIdentity`
  - 若 `chunk_id` 缺失但具备 `object_id/version/chunk_index`，则派生 `chunk_id`
  - 检查 `request.offset` 与 `identity.offset` 的冲突
- 将 transfer 层请求映射到 `StorageNodeClient::WriteChunk`
- 把 `status`、`error_detail`、`retry_after_ms`、`durable`、`already_exists`、`metadata` 等结果回填到 transfer 层结果

3. `ReadChunk`
- 校验 `request_id`、target endpoint
- 规范化 `ChunkIdentity`，确保最终有可读的 `chunk_id`
- 将 transfer 层请求映射到 `StorageNodeClient::ReadChunk`
- 把 `status`、`error_detail`、`retry_after_ms`、`actual_checksum`、`payload`、`verified`、`metadata` 等结果回填到 transfer 层结果

4. channel 复用
- 对 `endpoint -> grpc::Channel` 做轻量缓存
- 每次调用基于缓存 channel 构造本次所需的 `StorageNodeClient`
- 不在 transfer adapter 中缓存对象状态、manifest 或可见性判断

5. retryable 诊断
- 复用 `IsRetriableStatus(...)` 给出 `retryable` 标记
- 保持幂等重试依赖 `request_id + chunk identity` 的上层约束，不在 adapter 层发明 upload/download 编排重试

## 3. 如何保持 StorageNode data-plane、object visibility 和 payload boundary

当前实现保持了这三条边界：

1. StorageNode data-plane 边界
- 只通过 `StorageNodeClient::WriteChunk` / `ReadChunk` 与 StorageNode 交互
- 不直接读写 StorageNode 本地 chunk 文件、索引、publish 状态或 chunk store 内部结构

2. object visibility 边界
- adapter 不判断对象是否 `COMMITTED` 可见
- 即使 StorageNode 返回 live chunk / readable chunk，也不把它解释为对象已提交或可下载
- 对象可见性仍必须由 MetadataNode / COMMITTED manifest 决定

3. payload boundary
- `WriteChunk` / `ReadChunk` 只处理单次 bounded chunk payload
- 不引入整文件常驻内存接口
- 不把 payload 引入 metadata / Raft control-plane

## 4. 是否发现不合理点 / 警告 / 风险

有两点需要记录：

- `T033` 当前头文件只有抽象接口，没有 concrete adapter 或 factory 声明，导致 `T034` 无法仅靠 `.cpp` 落地。因此本任务对 `storage_transfer_client.h` 做了最小必要补口：增加 `StorageTransferClientConfig` 和 `CreateGrpcStorageTransferClient(...)`。
- 当前工作区 diff 中存在本任务之外的外部差异：
  - `tasks.md` 中可见 `T030` 也已被勾选
  - `module-notes.md` 中也存在与 `T029/T030/T033` 相关的已有补充
  - 本任务未修改或验证这些外部任务，只完成 `T034` 所需的最小实现和说明

## 5. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

未修改 `common-risk-notes.md`，未修改 `risk-register.md`。

## 6. 验证命令和结果

已执行：

1. diff 检查

```bash
git diff -- modules/store/transfer/storage_transfer_client.cpp modules/store/transfer/storage_transfer_client.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md
git status --short -- modules/store/transfer/storage_transfer_client.cpp modules/store/transfer/storage_transfer_client.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t034-storage-transfer-client-impl.md
```

结果：

- `storage_transfer_client.cpp` 已新增
- `storage_transfer_client.h` 做了最小必要接口补充
- `module-notes.md` 已同步最小实现边界
- `tasks.md` 已将 `T034` 勾选为完成
- diff 中同时可见工作区内已有的 `T030` 等外部差异，这些不是本任务新增

2. 最小相关 target 构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core' || echo "build lock busy, skip build in this window"
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --build --preset debug-ninja-safe --target raft_core --clean-first' || echo "build lock busy, skip build in this window"
```

结果：

- 第一次 configure/build 成功拿到锁，但 target 处于最新状态，输出 `ninja: no work to do.`
- 为确认新文件确实编译进目标，又执行了 `--clean-first` 的最小 target 重编
- 重编成功，日志中明确出现：
  - `Building CXX object ... modules/store/transfer/storage_transfer_client.cpp.o`
  - `Linking CXX static library libraft_core.a`

## 结论

- `T034` 已完成。
- 当前实现保持在 transfer -> StorageNode data-plane adapter 边界内，没有越权到 object visibility、MetadataService 或编排层。
- 可以进入后续 US1 任务，尤其是依赖该 adapter 的 `T032`、`T035`、`T036`、`T037`。 
