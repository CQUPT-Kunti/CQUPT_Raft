# T057-FIX GC Snapshot Streaming Save

## 修改文件

- `modules/store/maintenance/gc_task_store.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_garbage_collector_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 把 `GarbageCollectorTaskStore::SaveSnapshot()` 从“先序列化整份 snapshot payload，再一次性写入”改成了 streaming save。
- 保留原有按 `task_id` 的 deterministic sorting。
- 保留原有 `GC_TASK_STORE_V1` 文本格式和 `LoadSnapshot()` 恢复语义。
- 补充 `storage_garbage_collector` 测试，覆盖：
  - snapshot 文件头和 `count` 行兼容
  - 多 task 保存顺序稳定
  - 128 个 task 的保存与恢复
  - `task_id/chunk_id/reason/metadata_boundary/attempts/last_error/state` 等关键字段恢复不丢失

## 当前保存方式改成了什么样

- 先复制并排序 task 列表
- `OpenStagingWriter(...)`
- 依次 `Append`：
  - `GC_TASK_STORE_V1`
  - `count <N>`
  - 每个 task 的单行序列化结果
- 然后继续：
  - `Flush(kDataAndMetadata)`
  - `Close()`
  - `PublishStagedFile(...)`
  - `SyncDirectory(...)`

## 是否仍生成完整 gc/tasks.snapshot

- 是
- 当前只是把保存路径改成 streaming append；磁盘上仍生成一份完整的 `gc/tasks.snapshot`

## 是否保留排序

- 是
- 仍按 `task_id` 做 deterministic sorting

## 是否保持格式兼容

- 是
- 仍保持 `GC_TASK_STORE_V1` header、`count` 行和逐 task 文本行格式
- `LoadSnapshot()` 无需改动即可恢复新保存的 snapshot

## 是否更新 module-notes.md

- 是

## common-risk-notes.md 变更

- 已更新 `T057` 风险描述
- 明确：
  - 保存阶段不再先拼整份 payload，内存峰值风险已收缩
  - 仍保留 whole-snapshot rewrite 的磁盘写放大风险
  - 仍保留 schema migration 缺失、多进程共享 `persistence_root` 无协议、Windows directory durability 待验证等风险

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：未单独保存 build 日志
- `ctest --test-dir build/linux -R "garbage_collector|storage_garbage_collector|gc_restart|restart_resume" --output-on-failure 2>&1 | tee tmp/007/t057-gc-snapshot-streaming-save.log`
  - PASS
  - 日志路径：`tmp/007/t057-gc-snapshot-streaming-save.log`
  - 说明：实际匹配到的测试名为 `storage_garbage_collector`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## 是否可以继续进入 T058

- 可以
