# T008 Chunk Id Helper

- 修改文件
  - `modules/store/common/store_types.h`
  - `modules/store/common/store_types.cpp`
  - `tests/store_types_test.cpp`
  - `tests/support/store_test_utils.h`
  - `specs/007-storage-node-data-plane/research.md`
  - `specs/007-storage-node-data-plane/tasks.md`
  - `specs/007-storage-node-data-plane/task-reports/t008-chunk-id-helper.md`

- 做了什么
  - 在 `modules/store/common/store_types.h/.cpp` 中实现 `chunk_id` 相关 helper，包括 `object_id` 校验、`chunk_id` 生成、`chunk_id` 解析和合法性校验。
  - 采用 `object_id~version~chunk_index` 的规范化编码，显式拒绝空 `object_id`、路径逃逸、路径分隔符、危险字符、`version=0`、非规范数字编码和 `chunk_index` 溢出。
  - 更新 `tests/store_types_test.cpp`，覆盖合法生成、合法解析、合法校验、非法 `object_id`、非法 `version`、非法 `chunk_index`、路径逃逸和边界值。
  - 按当前 store 数据面约定，将 `modules/store` 相关代码与直接依赖它的测试命名空间从 `raftdemo` 调整为 `storedemo`。
  - 将 `research.md` 中与本地文件布局冲突的 `:` 示例修正为与实现一致的安全分隔符示例。
  - 在 `tasks.md` 中将 T008 标记为已完成。

- 验证命令和结果
  - `cmake --build --preset debug-ninja-low-parallel`：PASS
  - `ctest --test-dir build/linux -R "store_types" --output-on-failure`：PASS
  - `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`：PASS

- 是否通过 T008
  - 通过。

- 是否可以进入 T009
  - 可以。

- 当前任务发现的不合理点 / 警告 / 风险
  - `.specify/scripts/bash/check-prerequisites.sh` 仍会把当前 feature 解析到 `specs/006-remove-kv-metadata-state-machine`，本任务继续按用户指定的 `specs/007-storage-node-data-plane` 执行。

- 是否修正了高频文档，为什么
  - 是。修改了 `specs/007-storage-node-data-plane/research.md`，因为其中使用 `:` 作为 `chunk_id` 示例分隔符，会与“chunk_id 需适合作为本地文件布局一部分”的要求冲突；修改了 `specs/007-storage-node-data-plane/tasks.md`，仅用于将 T008 标记为完成。

- common-risk-notes.md 新增/删除/解决了哪些项
  - 无新增、无删除、无解决项。
