# T095 Manifest ChunkRef Compatibility

## 修改文件

- `tests/metadata_manifest_test.cpp`
- `tests/storage_upload_integration_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t095-manifest-chunkref-compatibility.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 读取并核对了 `proto/common.proto`、`proto/metadata.proto`、`proto/storage_node.proto`、`MetadataStateMachine` snapshot save/load、`MetadataServiceImpl` 的 `CommitObject` / `HeadObject` / `ListObjects` 映射，以及 storage upload/read 集成测试。
- 重写 `tests/metadata_manifest_test.cpp`，新增 metadata/common proto descriptor 边界断言和 `MetadataStateMachine` snapshot round-trip 断言。
- 扩展 `tests/storage_upload_integration_test.cpp`，新增真实 payload marker 集成用例，确认 durable chunk payload 只留在 `LocalDiskChunkStore`，不会进入 serialized commit command 或 metadata snapshot。

## ObjectRecord.chunks / ChunkRef 兼容性检查结论

- `ObjectRecord.chunks` 仍然只保存 `ChunkRef` 列表。
- `ChunkRef` 仍然只保存 `chunk_id`、`offset`、`size`、`replica_nodes`、`checksum`。
- `CommitObjectRequest.chunks` 仍然只承载 manifest facts，不承载 payload bytes。

## metadata manifest 是否只保存 chunk refs

- 是。
- `MetadataServiceImpl::FillChunkRef()` / `FillObjectRecord()` 只把 manifest facts 映射到 proto。
- `MetadataStateMachine::SaveSnapshot()` / `LoadSnapshot()` 的 `ObjectRecord` 路径只读写 bucket/object identity、state、etag、chunk refs 和时间字段，不写 chunk payload。
- upload integration 中的 committed manifest、`HeadObject`、`ListObjects`、`FindChunkRefs()` 都只暴露 chunk refs / manifest facts。

## 是否发现 payload 进入 metadata / Raft / snapshot

- 在本次 007 manifest / `ObjectRecord` / `ChunkRef` / `CommitObject` / `HeadObject` / `ListObjects` / `MetadataStateMachine` snapshot 检查范围内，未发现 payload 进入 metadata / Raft / snapshot。
- 新增的 payload marker 用例证明：
  - payload 可成功落入 `LocalDiskChunkStore` 并可从 data-plane `ReadChunk` 读回；
  - 同一个 marker 不会出现在 serialized commit metadata command；
  - 同一个 marker 不会出现在 metadata snapshot 文件。

## 是否修改生产代码

- 否。

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel 2>&1 | tee tmp/007/t095-build.log`
  - PASS
- `ctest --test-dir build/linux -R "MetadataManifestTest|storage_upload|storage_read" --output-on-failure 2>&1 | tee tmp/007/t095-manifest-chunkref-compatibility.log`
  - PASS
- 说明：
  - 原始建议过滤器 `metadata_manifest|storage_upload|storage_read` 未命中 gtest-discover 生成的 `MetadataManifestTest.*` 用例名。
  - 因此实际按真实测试名调整为 `MetadataManifestTest|storage_upload|storage_read`，覆盖了 2 个 manifest 用例与 4 个 storage upload/read 相关测试。

## Windows 验证判断

- 当前无 Windows 编译环境。
- 本次不宣称 Windows PASS。

## 是否通过 T095

- 是。

## 是否可以进入 T096

- 可以。

## 当前任务发现的不合理点 / 警告 / 风险

- 未发现 007 新引入的 manifest/payload 越界问题。
- 这轮检查聚焦 007 StorageNode data-plane 与 metadata manifest 边界，没有扩展成额外的 no-KV 或全量 metadata 回归。

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：无
- 保留：既有风险全部保留，`common-risk-notes.md` 本轮未修改
