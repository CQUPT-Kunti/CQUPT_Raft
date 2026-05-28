# T012 Durable File Contract Test

## 修改文件

- `tests/store_durable_file_test.cpp`
- `tests/CMakeLists.txt`
- `modules/store/io/module-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/store_durable_file_test.cpp`，为 `storedemo::DurableFile` / `DurableFileWriter` 提供跨平台契约测试骨架。
- 测试使用最小 fake durable file / writer，不依赖 Linux 或 Windows 真实平台实现。
- 覆盖了 `DurableFileErrorCode` 到 `StorageNodeStatusCode` 的共享错误映射和 retryable 分类。
- 固定了 required durability operation 的契约：
  - flush / publish / directory sync 可以显式失败或返回 unsupported；
  - 但不能以 `kOk` 且未到达 durable boundary 的 silent no-op success 冒充成功。
- 在 `tests/CMakeLists.txt` 中注册 `store_durable_file`，并打上 `storage-node;storage-node-cross-platform;platform-neutral` 标签。
- 补充 `modules/store/io/module-notes.md` 的测试边界说明，明确 T012 只验证接口契约，不证明平台 durability 已实现。
- 将 `tasks.md` 中 T012 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_durable_file" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS
- `ctest --test-dir build/linux -N -L platform-neutral`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## 是否通过 T012

- 通过

## 是否可以进入 T013

- 可以进入 T013

## 当前任务发现的不合理点 / 警告 / 风险

- 本任务未发现新增公共风险。
- 当前测试只固定接口契约，不代表 Linux / Windows durable file 语义已经被真实验证；真实平台证明仍留给 T013 / T014。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T012 标记为完成。

## 是否新增/更新 module-notes.md

- 是。更新了 `modules/store/io/module-notes.md`，补充 durable file 契约测试边界说明。

## common-risk-notes.md 新增/删除/解决了哪些项

- 无新增项。
- 无删除项。
- 无已解决项。
