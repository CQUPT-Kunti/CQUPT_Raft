# T076 Orphan Metadata Driven GC

## 修改文件

- `tests/storage_delete_gc_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t076-orphan-metadata-driven-gc.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在 `storage_delete_gc_test.cpp` 继续扩展真实 `GarbageCollector + metadata-driven safety checker + LocalDiskChunkStore::DeleteChunk` 边界测试。
- 新增一组 candidate/task 边界断言，固定 `pending-timeout / failed-upload / abort / deleted-object` 生成的 cleanup candidate 在转换成 `GarbageCollectorTask` 后都保留 `metadata_boundary`。
- 新增 pending-timeout orphan 场景：
  - 本地已有 orphan chunk
  - metadata 中另一个 committed live manifest 仍引用同一个 `chunk_id`
  - safety checker 返回 conflict
  - delete handler 不得运行
- 新增 failed-upload orphan 场景：
  - 本地 orphan chunk 经 `BuildFailedUploadCleanupCandidates(...)` -> `CleanupCandidateToGarbageCollectorTask(...)`
  - 只有在 metadata safety checker 放行后才允许真实 `DeleteChunk`
  - 删除后 `ReadChunk` 不可读，`StatChunk` 返回 `kDeleted`
- 没有修改生产 GC / store 逻辑，没有实现 Repair / Rebalance / Scrub。

## orphan chunk metadata-driven GC 覆盖场景

- 本地 orphan chunk 不能因为“本地看起来可删”就直接删除
- pending-timeout orphan 进入 `GarbageCollector` 后仍必须先过 metadata safety check
- metadata committed live manifest 仍引用同一 `chunk_id` 时，GC 必须阻止 delete handler
- metadata 不再引用时，failed-upload orphan 才允许 delete handler 删除
- deleted-object cleanup candidate 继续沿用既有 live-manifest safety gate
- repeated cleanup / repeated delete 幂等边界继续保留
- pending / failed-upload / abort / deleted-object candidate 都保留 `metadata_boundary`
- cleanup 后 `ReadChunk` 与 `StatChunk` 行为符合当前 `LocalDiskChunkStore` delete contract

## metadata safety checker 与 delete handler 当前边界

- safety checker 决定“是否允许删除”
- delete handler 只在 `status == kOk` 时运行
- blocked 场景下：
  - checker 运行
  - handler 不运行
  - task 进入 `Failed`
- allowed 场景下：
  - checker 先运行
  - handler 再调用真实 `LocalDiskChunkStore::DeleteChunk`
  - task 进入 `Completed`
- 当前没有扩展成 metadata freshness 协议、跨节点协调、repair / scrub / rebalance

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 `RaftNode::ProposeMetadata()` 保存 payload
- 不把 object payload 写入 metadata / Raft / snapshot
- 测试中只读取 `MetadataStateMachine` 的 live-manifest facts 做 safety check
- orphan cleanup 不改变 metadata object state

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务使用 `MakeChunkPayload(...)` 和已有 upload/store test helper

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_delete_gc|storage_node_recovery|orphan|metadata_driven_gc" --output-on-failure 2>&1 | tee tmp/007/t076-orphan-metadata-driven-gc.log`
  - PASS
  - 实际匹配到的测试名为 `storage_node_recovery`、`storage_delete_gc`
  - 日志路径：`tmp/007/t076-orphan-metadata-driven-gc.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T076 本身是平台无关的 metadata-driven GC 边界测试
- 当前无 Windows 编译环境，不伪造 Windows PASS
- 若后续涉及真实文件删除语义，Windows delete / sharing violation 风险仍待实机验证

## 是否通过 T076

- 是

## 是否可以进入 T077

- 可以
- T077 应继续做 US5 低并发恢复/跨平台验证汇总，不要把 T076 扩成 GC 实现演进或 US6 任务

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 safety checker 的正确性仍依赖调用方提供的 metadata facts 是否足够新鲜。
- T076 固定的是“delete handler 不能绕过 safety gate”，不是 metadata snapshot 新鲜度或跨节点竞态完全收口。
- Windows 删除语义、sharing violation 和真实断电级 durability 仍未实机验证。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只补测试和文档，没有修改 `modules/store/*` 生产实现

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T076 完成并记录真实修改范围
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T076 风险条目，并保留 metadata freshness / Windows 删除语义等未解风险

## common-risk-notes.md 读取结果

- 已读取
- Windows rename/delete/sharing violation 风险仍保留
- 真实断电级 durability 风险仍保留
- metadata fact 新鲜度、delayed retry scheduler、repair/rebalance/scrub 风险仍保留
- prerequisites 脚本误指向 006 的问题仍保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T076`，记录 orphan cleanup 仍依赖 metadata facts 新鲜度与 Windows 删除语义后续验证
- 删除：
  - 无整项删除
- 保留：
  - `T014/T023/T025/T026/T049/T055/T056/T057/T068/T069/T070/T071/T072/T073/T074/T075` 等后续风险继续保留
- 收缩：
  - 无额外整项收缩；本轮只关闭“orphan chunk metadata-driven GC 边界测试缺失”这一层
