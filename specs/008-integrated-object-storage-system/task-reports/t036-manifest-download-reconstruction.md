# T036 任务报告：manifest-driven download reconstruction

## 1. 修改了哪些文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t036-manifest-download-reconstruction.md`

## 2. manifest-driven download reconstruction 和 final checksum verification 做了什么

- 在 `BasicDownloadTransferSession::Execute(...)` 中补全了真实下载路径：
  - 先通过 ViewNode 发现 MetadataNode。
  - 再通过 Metadata adapter 获取 `COMMITTED manifest`。
  - 对 manifest 做最小一致性校验：
    - chunk 按 `offset/chunk_index` 连续。
    - `sum(chunk.size) == object size`。
    - 非空对象必须带对象级 checksum facts。
    - 非空 chunk 必须带 chunk checksum 和 replica node_id。
  - 通过 ViewNode `DiscoverStorage` 获取 `node_id -> endpoint` 的观测快照，但只用于 data-plane endpoint 解析。
  - 对每个 chunk 调用 `StorageTransferClient::ReadChunk(...)`，并用 `TransferChecksumState` 做逐 chunk checksum 校验。
  - 以临时文件 `destination_path.<request_id>.part` 作为下载输出，按 manifest offset 写入，避免整对象常驻内存。
  - 所有 chunk 成功后，做最终对象级 checksum 校验，并与 manifest / 用户期望 checksum facts 比对。
  - 最终校验成功后再 publish 到 `destination_path`，失败则返回明确诊断。
- 在 `DiscoverStorageTargets(...)` 内部补了 `require_writable` 参数：
  - upload 仍要求可写节点。
  - download 改为 discovery readable candidate，不再错误依赖 “必须可写”。

## 3. 如何处理 checksum mismatch、输出文件失败和对象可见性边界

- checksum mismatch：
  - `StorageNode` 返回 `kChecksumMismatch` / `kCorrupted` 时映射为 `ObjectTransferStatusCode::kChecksumMismatch` 并立即失败。
  - 即使 StorageNode 读成功，也会在 transfer 侧再次按 manifest checksum 做 chunk 校验。
  - 最终对象 checksum 与 manifest 或 `expected_object_checksum` 不一致时同样立即失败。
- 输出文件失败：
  - 下载写入先落到临时文件，不直接写最终输出路径。
  - `destination_path` 已存在时返回 `kConflict`，避免把结果静默覆盖。
  - seek/write/flush/publish 任一步失败都返回 `kIoError`，并清理临时文件，不把部分文件声明为成功。
- 对象可见性边界：
  - download 只接受 MetadataNode 返回的 `COMMITTED manifest`。
  - ViewNode 只负责 discovery / observation，不决定对象可见性。
  - StorageNode 只返回 chunk payload 与校验事实，不提供 manifest authority。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前下载路径对每个 chunk 只选择第一个可解析的 replica endpoint；尚未实现多副本读重试或 checksum mismatch 后的副本切换，这仍是后续增强点。
- 当前实现对最终输出路径采取“已存在即失败”的保守策略，避免覆盖已有文件；如果后续 CLI 需要 `--force` 类行为，应在 `storage_client` 层显式建模，而不是在 transfer 内静默替换。
- 当前 `DiscoverStorage` 仍依赖 ViewNode 的 live snapshot；如果 manifest 中 replica 已提交但节点当前未被观测到，download 会以 discovery failure 结束。这符合 observation-only 边界，但需要后续整体集成测试继续验证。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。

## 6. 验证命令和结果

### diff 检查

```bash
git diff -- modules/store/transfer/object_transfer.cpp modules/store/transfer/object_transfer.h modules/store/transfer/metadata_transfer_client.h modules/store/transfer/metadata_transfer_client.cpp modules/store/transfer/storage_transfer_client.h modules/store/transfer/storage_transfer_client.cpp modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t036-manifest-download-reconstruction.md
```

- 结果：已确认本任务改动集中在 `object_transfer.cpp`、`module-notes.md`、`tasks.md` 和当前报告文件；未改 proto、app 入口和测试。

### 最小构建验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target raft_core'
```

- 结果：`PASS`
- 说明：`raft_core` 成功完成 configure 和 build，`modules/store/transfer/object_transfer.cpp` 已通过最小相关 target 编译验证。

### 测试执行

- 本任务未额外运行 `ctest`。
- 原因：`integrated_object_storage_e2e` 的正式 target 接入在 `T039`，当前先完成 transfer 层最小构建验证；端到端下载验收需等 `T037/T039` 后继续补齐。
