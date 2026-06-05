# T050 Delete Chunk Contract Test

## 修改文件

- `tests/storage_delete_chunk_contract_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t050-delete-chunk-contract-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_delete_chunk_contract_test.cpp`
- 在测试内实现 test-only `DeleteChunkContractAdapter`
- 用真实 `LocalDiskChunkStore` 固定单删语义，用 test-only batch 聚合固定 `BatchDeleteChunks` 的逐项结果和 retry 分类
- 在 `tests/CMakeLists.txt` 注册 `test_storage_delete_chunk_contract` / `storage_delete_chunk_contract`
- 将 `tasks.md` 的 T050 实际测试路径修正为 `tests/storage_delete_chunk_contract_test.cpp`
- 在 `common-risk-notes.md` 新增 T050 风险，明确当前只有 contract test，没有生产删除 RPC

## DeleteChunk / BatchDeleteChunks contract 覆盖场景

- `DeleteChunk` 删除 live chunk 成功后，`ReadChunk` 不再成功
- `DeleteChunk` 对 missing chunk 返回幂等成功，并显式 `already_missing`
- `DeleteChunk` 对 deleted chunk 重复调用保持幂等
- `DeleteChunk` 对 quarantined / corrupted chunk 当前固定为可删除，并在删除后不可再读
- `DeleteChunk` 的 checksum mismatch 返回明确错误，不误删 live chunk
- `DeleteChunk` 不决定 object 是否 deleted；删除 data-plane chunk 后 metadata `HeadObject` / `ListObjects` 仍保持 committed 可见
- `BatchDeleteChunks` 覆盖 live、missing、deleted、retryable failure、non-retryable failure 混合批次
- `BatchDeleteChunks` 为每个 chunk 返回独立结果
- partial batch result 可区分成功、幂等成功、retryable failure、non-retryable failure
- retryable failure 后续可单独重试成功，且不污染已成功删除项

## test-only adapter 与生产 RPC 当前边界

- 当前只实现 test-only adapter，不实现生产 `StorageNodeService::DeleteChunk`
- 当前只实现 test-only adapter，不实现生产 `StorageNodeClient::DeleteChunk`
- 当前不实现 proto `DeleteChunk` / `BatchDeleteChunks` 字段
- 当前不实现生产 `GarbageCollector`
- 当前 batch 结果和 retry 分类只是 contract test 边界，真正的线协议和 service/client 映射仍待 T051-T053

## 是否使用 tests/test_file/test_file.deb

- 是

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存
- `ctest --test-dir build/linux -R "delete_chunk_contract|local_disk_chunk_store" --output-on-failure 2>&1 | tee tmp/007/t050-delete-chunk-contract.log`
  - PASS
  - 日志路径：`tmp/007/t050-delete-chunk-contract.log`
  - 说明：本次有意未执行 `storage_node_service`，因为当前不存在生产 delete RPC，实现上不应把 T050 扩展到 service 删除路径；改为执行新注册的 `storage_delete_chunk_contract` 加上依赖的 `local_disk_chunk_store`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志摘要

- 本次验证最终 PASS，无失败项

## Windows 验证判断

- T050 当前只在 Linux 环境验证 delete contract
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未新增 `T050-WIN`

## 是否通过 T050

- 是

## 是否可以进入 T051

- 可以

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`
- `tasks.md` 原 T050 路径写成了 `tests/storage_node_service_test.cpp`，与当前真实 contract 测试落点不一致；本次已修正
- T050 只固定了 test-only 删除 contract，不关闭生产 `DeleteChunk` / `BatchDeleteChunks` RPC、生产 GC、restart cleanup、Windows 待验证等风险
- batch retry 语义目前只存在于 test-only adapter；真正接 proto/service/client 时必须严格保留逐项结果和 retry 分类 contract
