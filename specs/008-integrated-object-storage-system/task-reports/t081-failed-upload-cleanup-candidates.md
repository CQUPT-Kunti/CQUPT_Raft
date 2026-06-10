# T081 任务报告

## 1. 修改了哪些文件

- `modules/store/transfer/object_transfer.cpp`
- `modules/store/transfer/object_transfer.h`
- `modules/store/transfer/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t081-failed-upload-cleanup-candidates.md`

未修改：

- `modules/store/maintenance/garbage_collector.h`
- `common-risk-notes.md`
- `specs/008-integrated-object-storage-system/risk-register.md`

## 2. failed upload sessions cleanup candidate emission 做了什么

- 在 `UploadObjectResult` 中新增 `cleanup_candidates`，直接承接 failed upload 的 `CleanupCandidate` 集合。
- 保留并继续使用 `cleanup_candidate_possible`，用于表达“存在 cleanup 风险”，包括已确认 durable 的 orphan chunk 和 retryable/uncertain write 风险。
- upload 主链路补齐到：
  - 首遍 bounded 读取源文件，计算 chunk/object checksum facts
  - `CreateWritePlan`
  - 二次 bounded 读取源文件并执行 `StorageTransferClient::WriteChunk`
  - 收集 durable chunk facts
  - `CommitObject`
- 若失败发生在 chunk durable 成功之后，会把 durable chunk facts 转换成 `BuildFailedUploadCleanupCandidates(...)` 可消费的 `CleanupCandidate`。
- 若失败是 retryable / uncertain write，且无法完整确认 durable 事实，也会保留 `cleanup_candidate_possible=true` 和明确诊断，不把失败静默吞掉。

## 3. 哪些失败场景会产生 cleanup candidates

以下场景会生成或触发 failed-upload cleanup candidate：

- 某个 chunk 的部分副本 `WriteChunk` 成功，但未达到 `minimum_successful_writes`
- 某个 chunk 已达到最小成功写入，后续 chunk 失败，导致整次 upload 不能进入 `CommitObject`
- 所有 chunk durable 完成后，`CommitObject` RPC 失败或返回非成功状态
- upload 第二遍读取时发现 chunk 顺序/offset/size/checksum 与首遍 prepared facts 不一致，且此前已经存在 durable chunk
- 第二遍 upload 中途发生文件 IO 错误，且此前已经存在 durable chunk

以下失败只会返回失败，不会生成 chunk cleanup candidate：

- 参数校验失败
- 首遍 checksum/reader 失败且尚未发生任何 chunk 写入
- `CreateWritePlan` 失败
- `DiscoverStorage` 失败且尚未发生任何 chunk 写入

## 4. 如何避免把 cleanup candidate 误解释为 COMMITTED 对象或误删 live chunk

- `cleanup_candidates` 只在 upload 失败路径生成；成功 `CommitObject` 后会清空。
- `committed=true` 只在 `MetadataTransferClient::CommitObject` 明确成功且对象可见时设置。
- `cleanup_candidates` 使用 `CleanupCandidateSource::kFailedUpload` / `GarbageCollectionReason::kFailedUploadCleanup`，明确语义是“失败上传后的清理候选”，不是已提交 manifest。
- candidate 的生成只基于 transfer 已知 durable chunk facts；最终是否允许删除，仍必须由 T080 的 cleanup hook 和 metadata-driven safety checker 决定。
- 本任务没有直接删除 chunk，没有绕过 MetadataNode authority，也没有修改对象 `PENDING/COMMITTED/DELETED` 可见性语义。

## 5. 如何保持失败返回、request_id、node_id、chunk_id 等诊断信息

- 扩展了 upload 失败路径的 `Fail(...)`，现在可携带：
  - `request_id`
  - `chunk_id`
  - `chunk_index`
  - `offset`
  - `node_id`
  - `endpoint`
  - `retryable`
- 为每次 `StorageNode WriteChunk` 追加了 data-plane 诊断，成功和失败都保留目标节点与 chunk 身份。
- 对 retryable / uncertain write 会额外写入说明性诊断，避免只有一个布尔标记却没有上下文。

## 6. 是否发现不合理点 / 警告 / 风险

- 当前 `MetadataTransferClient::CreateWritePlan` 仍未稳定提供显式 chunk placement；因此 `object_transfer` 目前会优先使用 write plan 中的 `candidate_nodes`，若缺失则回退到 ViewNode `DiscoverStorage` 结果的稳定排序前 N 个目标。这保持了可运行性，但后续若 metadata service 返回更精确 placement，应继续以 metadata 为准。
- upload 为了保持 bounded memory，采用“首遍算 checksum / 二遍写 chunk”的两遍文件读取路径。语义正确，但大文件会增加一次本地读放大；后续若引入更强的 streaming plan/commit 编排，可再优化。
- 当前相关 transfer/recovery 测试 target 还未接入 CTest 正则，本任务只有编译级验证，没有专项自动化测试结果。

## 7. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

## 8. 验证命令和结果

### diff 检查

命令：

```bash
git diff -- modules/store/transfer/object_transfer.cpp modules/store/transfer/object_transfer.h modules/store/maintenance/garbage_collector.h modules/store/transfer/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t081-failed-upload-cleanup-candidates.md
```

结果：

- PASS
- 变更集中在 T081 指定文件

### 最小相关 build

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_client'
```

结果：

- PASS
- 成功编译 `modules/store/transfer/object_transfer.cpp`
- 成功链接 `raft_core` 与 `storage_client`

### 相关测试筛选

命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R "object_transfer|storage_transfer|integrated_object_storage_recovery" --output-on-failure'
```

结果：

- 当前返回 `No tests were found!!!`
- 说明当前没有命中该正则的已接入测试；本窗口未获得 T081 专项测试结果
- 未为此主动修改 CMake target，符合 “T084 负责 wire recovery/concurrency test targets” 的任务边界

## 结论

- T081 已完成实现与最小验证。
- 可以进入后续 US6 任务，尤其是：
  - `T083` bounded concurrency controls
  - `T084` recovery/concurrency test target 接入
