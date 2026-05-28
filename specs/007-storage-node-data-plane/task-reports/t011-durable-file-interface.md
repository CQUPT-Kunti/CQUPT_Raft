# T011 Durable File Interface

## 修改文件

- `modules/store/io/durable_file.h`
- `modules/store/io/durable_file.cpp`
- `modules/store/io/module-notes.md`
- `modules/store/chunk/chunk_store.h`
- `modules/store/chunk/module-notes.md`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `modules/store/io/` 子模块，定义 `storedemo::DurableFile`、`storedemo::DurableFileWriter` 抽象接口。
- 定义 durable file 层请求/响应结构，覆盖路径规范化、staging writer、append、flush、close、atomic publish、directory sync。
- 定义 `DurableFileErrorCode`，补充 `partial_write`、`path_invalid`、`atomic_publish_failed`、`directory_sync_failed` 等文件层细粒度错误，并提供到 `StorageNodeStatusCode` 的共享映射。
- 在 `raft_core` 中接入 `modules/store/io/durable_file.cpp`，保证接口和映射函数参与主库编译。
- 清理 `modules/store/chunk/chunk_store.h` 中的解释性注释，把说明收敛到模块文档。
- 更新 `modules/store/chunk/module-notes.md`，让 chunk 模块说明与当前 `module-notes.md` 约定保持一致。
- 修正 `tasks.md` 中 T010 仍写成 `modules/store/chunk/README.md` 的旧文档名，并将 T011 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_types" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## 是否通过 T011

- 通过

## 是否可以进入 T012

- 可以进入 T012

## 当前任务发现的不合理点 / 警告 / 风险

- 本任务未发现新增公共风险。
- `tests/support/store_test_utils.h` 的 checksum fixture 与生产 SHA-256 语义不一致问题仍然存在，本任务未处理。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`：
  - 将 T011 标记为完成；
  - 将 T010 的模块说明文档路径从旧的 `README.md` 修正为实际存在的 `module-notes.md`，避免后续按错误文件名查找模块文档。

## 是否新增/更新 module-notes.md

- 是。新增 `modules/store/io/module-notes.md`。
- 同时更新了 `modules/store/chunk/module-notes.md`，让 chunk 模块说明与当前 module-notes 约定和无代码注释风格保持一致。

## common-risk-notes.md 新增/删除/解决了哪些项

- 无新增项。
- 无删除项。
- 无已解决项。
