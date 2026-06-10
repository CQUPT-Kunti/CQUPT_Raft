# T037 任务报告：storage_client upload/download

## 1. 修改了哪些文件

- `apps/storage_client.cpp`
- `specs/008-integrated-object-storage-system/contracts/app-cli.md`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t037-storage-client-upload-download.md`

## 2. storage_client upload/download 做了什么

- 新增 `apps/storage_client.cpp`，实现了 `storage_client upload` 和 `storage_client download` 两个命令入口。
- CLI 支持解析：
  - upload：`--config --bucket --object --file`
  - download：`--config --bucket --object --out`
- 额外支持最小可选参数：
  - upload：`--object-id --request-id --chunk-size --replicas --min-writes --concurrency`
  - download：`--object-id --version --request-id --concurrency`
- CLI 会从 `--config` 指向的 cluster config 中最小提取：
  - `cluster_id`
  - ViewNode endpoint
  - 可选 `chunk_size_bytes`
  - 可选 `replica_count`
  - 可选 `minimum_successful_writes`
  - 可选 discovery / metadata / storage / commit timeout
- 为 upload 在未显式传入 `--object-id` 时提供稳定安全的默认 object_id 推导，避免要求用户手工发明 object identity。
- upload/download 都会生成默认 `request_id`，除非用户显式覆盖。

## 3. CLI 如何调用 transfer 层并保持 app thin boundary

- app 层只负责：
  - 参数解析
  - 最小配置读取
  - 装配 `ViewNodeClient`、`MetadataTransferClient` seed、`StorageTransferClient`
  - 创建 `ObjectTransfer`
  - 调用 `StartUploadSession(...)->Execute(...)` / `StartDownloadSession(...)->Execute(...)`
  - 输出摘要、诊断和退出码
- app 层没有重新实现：
  - ViewNode discovery
  - Metadata RPC
  - StorageNode chunk read/write
  - manifest-driven reconstruction
  - chunk checksum / final object checksum 逻辑
- app 层显式保持边界：
  - upload 只有在 transfer 返回 `committed=true` 时才声明成功
  - download 只有在 transfer 返回 `checksum_verified=true` 时才输出 integrity `PASS`
  - 不打印 payload 或 chunk bytes

## 4. 如何处理错误、退出码和 integrity/checksum 结果

- 参数错误返回 `2`
- config 读取/解析错误返回 `3`
- 传输类失败返回 `4`
- 当前 capability 不满足时返回 `5`
- 其他内部异常返回 `10`
- upload 失败时：
  - 输出 `request_id`
  - 输出 transfer status
  - 输出错误信息
  - 输出 diagnostics
  - 如果有 `write_plan`，会附带 object_id/version/size/chunk_count 作为 partial result，但不会误报成功
- download 成功时：
  - 输出 `request_id`
  - 输出 `object_id/version/size/checksum`
  - 输出 `integrity=PASS`
- download 失败时：
  - checksum mismatch、manifest 不可见、StorageNode 读取失败、输出文件 IO 失败等都会返回非零并输出诊断

## 5. 是否发现不合理点 / 警告 / 风险

- 发现一个前置实现缺口：当前 `ObjectTransfer` 的 upload 路径仍未真正完成 chunk write + `CommitObject`，因此 CLI 必须把“`result.ok()` 但 `committed=false`”视为失败，不能假装上传成功。
- 发现一个 target wiring 缺口：`storage_client` target 现已因为源文件存在而被 CMake 占位逻辑激活，但当前链接阶段缺少 `view_proto` 等依赖，导致 link fail；这属于 T038 的职责范围，不应在 T037 越界修复。
- 当前 config 读取器是 app 内的最小解析实现，主要面向 `cluster_id`、ViewNode endpoint 和少量 chunk/timeout 参数；后续如果统一 config loader 落地，应优先复用共享实现。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- apps/storage_client.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t037-storage-client-upload-download.md
```

- 结果：改动集中在 T037 允许的 CLI 文件、最小 contract 文档、transfer 模块说明、tasks 勾选和当前报告文件。

### storage_client 最小构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_client' || echo "build lock busy, skip build in this window"
```

- 结果：`cmake configure PASS`
- 结果：`apps/storage_client.cpp` 编译 PASS
- 结果：`storage_client` link FAIL
- 失败摘要：
  - 当前 target 链接阶段缺少 `view_proto` 相关符号，出现 `view::ViewNodeService::NewStub(...)` 和多项 `view::*` protobuf/grpc symbol undefined reference
  - 这说明 `storage_client` target 的依赖 wiring 尚未完整，属于 T038 的职责边界
- 说明：
  - 本任务没有修改 `CMakeLists.txt`
  - 本任务已经完成 CLI 源文件实现
  - 下一步进入 T038 后应补齐 target link dependencies，再重新验证 `storage_client`
