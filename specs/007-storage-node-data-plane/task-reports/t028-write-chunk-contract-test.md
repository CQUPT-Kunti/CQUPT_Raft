# T028 WriteChunk Contract Test

## 修改文件

- `tests/storage_write_chunk_contract_test.cpp`
- `tests/CMakeLists.txt`
- `modules/store/chunk/module-notes.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 test-only `WriteChunk` contract adapter 测试，底层复用真实 `LocalDiskChunkStore`
- 用真实 `BoundedStorageExecutor` admission 覆盖 `kOverloaded` 映射
- 用 pending `MetadataStateMachine` 场景固定“`WriteChunk` 成功只代表 chunk durable，不代表 metadata COMMITTED 可见”
- 将 T028 在 `tasks.md` 标记完成，并把测试路径纠偏为实际落点 `tests/storage_write_chunk_contract_test.cpp`
- 记录 `check-prerequisites.sh` 仍错误指向 `specs/006-remove-kv-metadata-state-machine`，本任务继续按用户指定的 `specs/007-storage-node-data-plane` 执行

## WriteChunk contract 覆盖场景

- durable success 后 chunk 可读，但 metadata pending 对象仍不可见
- 同 `request_id` + 同 chunk + 同 payload 重试安全
- 同 `request_id` + 同 chunk + 不同 payload 返回 conflict-like 状态且不覆盖已发布 chunk
- checksum mismatch 不写 live chunk
- same chunk + same content 重复写返回 success/`already_exists`
- same chunk + different content 返回 conflict-like 状态且保留原内容
- bounded executor admission 满时映射为 `kOverloaded`
- `timeout_ms` / `best_effort_cancel` 当前只固定为显式边界，不伪造运行中取消已实现
- 二进制 payload 使用 `tests/test_file/test_file.deb`

## 是否使用 tests/test_file/test_file.deb

- 是

## node-data 可视化目录路径和保留内容

- 路径：`node-data/t028-write-chunk-contract/`
- 测试开始前会清理该目录旧内容
- 测试结束后保留最新一次用例留下的 `chunks/live/` 与 `chunks/staging/` 目录结构，便于人工查看 durable 后的本地布局
- 当前验证后保留了 `chunks/live/5e/64/obj-t028-binary-fixture~1~0.chunk`，并保留对应 `chunks/staging/` 分片目录

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`：PASS
- `ctest --test-dir build/linux -R "write_chunk_contract|storage_node_service" --output-on-failure`：PASS
- `ctest --test-dir build/linux -R "storage_upload|local_disk_chunk_store|store_concurrency_stress" --output-on-failure`：PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index|store_executor" --output-on-failure`：PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`：PASS
- `ctest --test-dir build/linux -N -L storage-node`：PASS

## Windows 验证判断

- 本任务未新增 Windows 专属 publish/read/delete 生产实现，只增加 contract 层测试并复用现有 Linux 环境下的 `LocalDiskChunkStore`
- 因此本次不新增 `T028-WIN`
- 现有 `T014-WIN`、`T023-WIN`、`T025-WIN`、`T026-WIN` 仍覆盖真实 Windows 数据面语义待验证范围

## 是否通过 T028

- 是

## 是否可以进入 T029

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `check-prerequisites.sh` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，需要后续 speckit 工作流修正
- T028 只固定了 contract 语义，真实 proto / service / client 的 deadline/cancelled/error mapping 仍待 T029-T032 收口

## 是否修正了高频文档，为什么

- 是，更新了 `tasks.md`
- 原因：将 T028 标记完成，并把实际测试路径从 `tests/storage_node_service_test.cpp` 纠偏为本次真实新增的 `tests/storage_write_chunk_contract_test.cpp`

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/chunk/module-notes.md`
- 未更新 `modules/store/chunk/AGENTS.md`

## common-risk-notes.md 读取结果

- 已读取
- T018、T019、T021、T023、T024、T025、T026、T027 风险仍存在，T028 不足以关闭

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增：T028 service/client contract 接入风险
- 删除：无
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027
- 解决：无
