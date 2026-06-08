# T029 Object Transfer 接口报告

## 1. 修改了哪些文件

- `modules/store/transfer/object_transfer.h`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t029-object-transfer-interface.md`

未修改 `proto/`、测试文件和 app 入口。

## 2. object_transfer.h 定义了什么接口边界

本次新增 `modules/store/transfer/object_transfer.h`，只定义 transfer 编排层接口，不实现真实上传下载流程。头文件主要定义了：

- `ObjectTransferDirection`、`ObjectTransferStage`、`ObjectTransferStatusCode`
  - 统一表达 upload/download 生命周期阶段和传输侧错误边界
- `TransferObjectChecksumFacts`、`TransferChunkPlan`、`TransferCommittedChunk`
  - 锁定 metadata/control-plane 只流转 size/checksum/chunk_id/node_id/offset 等 facts
- `TransferWritePlan`、`TransferCommittedManifest`
  - 表达后续 `CreateWritePlan` 和 COMMITTED manifest-driven reconstruction 的边界
- `TransferSessionSnapshot`、`UploadObjectRequest`、`DownloadObjectRequest`
  - 表达单次传输 session 的输入、快照和结果边界
- `TransferChunkReader`
  - 定义 bounded chunk 读取接口，单次只返回单个 chunk buffer
- `TransferChecksumState`
  - 定义增量 checksum 状态接口，支持逐 chunk `Append` 和最终 `Finalize`
- `TransferSession`、`UploadTransferSession`、`DownloadTransferSession`
  - 定义 session 生命周期和 upload/download 执行边界
- `ObjectTransfer`
  - 定义与 `MetadataTransferClient`、`StorageTransferClient`、`ViewNodeClient` 的依赖注入和 `StartUploadSession` / `StartDownloadSession` 入口

## 3. 是否保持只定义接口、不实现真实上传下载

是。

- 本次只新增头文件声明和必要中文注释
- 没有新增 `object_transfer.cpp`
- 没有实现真实文件 chunking、StorageNode `WriteChunk` / `ReadChunk`
- 没有实现 MetadataNode `CreateWritePlan` / `CommitObject`
- 没有实现 ViewNode discovery 重试逻辑

这些实现留给 T030、T031/T032、T033/T034、T035/T036。

## 4. 是否发现不合理点 / 警告 / 风险

发现一个需要后续实现阶段明确守住的点：

- `TransferChunkReadResult` 使用 `std::string payload` 表达单个 chunk buffer
- 这只是“单块 bounded buffer”的接口表示，不是允许整文件常驻内存
- T030 必须显式保证单次读取大小受 `chunk_size` 约束，不能把完整对象读入一个 `payload`

此外：

- `ObjectTransfer` 当前直接依赖 `viewdemo::ViewNodeClient` 的注入边界
- 后续 T035 集成 discovery 时，必须继续把 leader hint / endpoint 当作候选观测信息，而不是 metadata authority

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

- `common-risk-notes.md` 未修改
- `risk-register.md` 未修改

原因：

- 本任务只定义 transfer 接口边界，没有改变协议语义、Raft 安全边界或持久化格式

## 6. 验证命令和结果

执行命令：

```bash
git diff -- modules/store/transfer/object_transfer.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t029-object-transfer-interface.md
printf '#include "store/transfer/object_transfer.h"\nint main() { return 0; }\n' | c++ -std=c++20 -Imodules -fsyntax-only -x c++ -
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core' \
  || echo "build lock busy, skip build in this window"
```

结果：

- `git diff`：确认本次修改聚焦在 transfer 接口头文件、必要模块说明、任务状态和任务报告
- `fsyntax-only` 头文件语法检查：PASS
- `raft_core` 最小相关构建：PASS
- 使用了构建锁，并成功获得 `/tmp/cqupt_raft_build.lock`
- 总耗时：约 8 秒
- 构建日志：`tmp/test-logs/t029-build-raft-core-safe-with-lock.log`

补充说明：

- 本次是 header-only 任务，`raft_core` 构建主要用于确认没有破坏现有最小相关目标
- `specs/008-integrated-object-storage-system/tasks.md` 中除本次将 `T029` 从 `[ ]` 改为 `[X]` 外，还存在进入本任务前就已存在的未提交状态变更：`T031`、`T033` 已为 `[X]`；本次未回退这些既有改动
- `modules/store/transfer/module-notes.md` 中与 `metadata_transfer_client.h`、`storage_transfer_client.h` 相关的说明已在本任务开始前存在未提交改动；本次只补充了 `object_transfer.h` 的接口边界段落
- 如果构建锁被占用，本窗口不会等待，也不会重复发起构建；会在结果中明确记录为“未执行 build，待统一验证”
