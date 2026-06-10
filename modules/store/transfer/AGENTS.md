# modules/store/transfer

## 目录职责

- `modules/store/transfer/` 是 008 阶段为 `storage_client` 真实 upload / download 预留的客户端传输编排目录。
- 这里负责：
  - 通过 ViewNode 发现 MetadataNode。
  - 调用 MetadataNode 获取 `WritePlan` 或 COMMITTED manifest。
  - 按 `WritePlan` 调用 StorageNode `WriteChunk`。
  - 收集 chunk write result，为 `CommitObject` 准备 chunk manifest facts。
  - 下载时按 manifest 读取 chunk、逐 chunk 校验 checksum、拼接输出文件，并校验最终对象 checksum。
  - 维护 bounded file chunking / checksum state / 传输侧错误边界。

## 不负责

- 不保存 object manifest 的一致性权威副本。
- 不决定对象是否 COMMITTED 可见。
- 不修改 Raft membership。
- 不参与 Raft quorum、leader election、commit 规则。
- 不让真实 payload、chunk bytes、完整文件数据进入 Raft log、Raft snapshot 或 metadata snapshot。
- 不把 ViewNode 的观测信息当成对象可见性依据。
- 不直接修改 StorageNode 本地 chunk 状态，必须通过 StorageNode contract 交互。
- 不实现整文件常驻内存的上传或下载路径。

## 修改入口

- 修改本目录前，先读根 `AGENTS.md`。
- 再读 `modules/store/AGENTS.md`。
- 再读本文件。
- 然后读 `module-notes.md` 和直接相关的 spec / plan / data-model / contract 文档。

## 修改规则

- 必须保持 client-side orchestration / adapter 边界，不把 metadata authority、StorageNode 内部实现或 app 业务分叉写入本目录。
- 真实文件数据只能走 StorageNode data-plane；Raft metadata control-plane 只接收 metadata、checksum、size、version、chunk manifest facts。
- checksum mismatch 必须显式失败并携带可诊断状态，不得静默成功、不得自动伪造成功结果。
- upload / download 必须保持 bounded memory，内存上界应由 chunk size、并发度和缓冲策略决定。
- 处理 `NOT_LEADER`、checksum mismatch、IO failure、discovery failure、StorageNode transient failure 时，必须保留清晰的传输侧错误边界。
- 新增关键结构、接口、状态、错误码、重试策略或平台差异说明时，必须同步更新 `module-notes.md`。
- 不在该目录写任务执行日志、调试流水账或临时结论。

## 重点关注

- `ObjectTransfer`
- `TransferSession`
- `MetadataTransferClient`
- `StorageTransferClient`
- chunk reader
- checksum state
- ViewNode discovery
- `WritePlan` / `CommitObject`
- COMMITTED manifest driven download
- bounded concurrency 和 bounded memory

## 相关文档

- `specs/008-integrated-object-storage-system/plan.md`
- `specs/008-integrated-object-storage-system/data-model.md`
- `specs/008-integrated-object-storage-system/contracts/app-cli.md`
- `specs/008-integrated-object-storage-system/contracts/metadata-object-flow.md`
- `specs/008-integrated-object-storage-system/contracts/storage-node-flow.md`
- `specs/008-integrated-object-storage-system/contracts/view-node-discovery.md`
