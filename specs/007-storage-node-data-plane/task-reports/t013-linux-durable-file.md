# T013 Linux Durable File

## 修改文件

- `modules/store/io/durable_file.h`
- `modules/store/io/durable_file.cpp`
- `modules/store/io/module-notes.md`
- `tests/store_durable_file_test.cpp`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 为 `storedemo::DurableFile` 增加 `LinuxDurableFile`，实现 Linux 下的 staging writer、`fdatasync` / `fsync`、same-filesystem `rename` publish、parent directory `fsync` 和 POSIX errno 错误映射。
- 补齐 root 内路径规范化与逃逸拒绝，确保 absolute path、`..` 和越界路径不会被当成合法 durable file 路径。
- 扩展 `tests/store_durable_file_test.cpp`，为 Linux 分支增加真实成功路径、路径拒绝、exclusive publish 冲突和缺失目录 sync 失败用例。
- 更新 `modules/store/io/module-notes.md`，说明 Linux durable file 已实现、Windows 路径仍留给 T014。
- 将 `tasks.md` 中 T013 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_durable_file" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## 是否通过 T013

- 通过

## 是否可以进入 T014

- 可以进入 T014

## 当前任务发现的不合理点 / 警告 / 风险

- 本任务未发现新的公共风险。
- 现有 `.specify` feature-dir 误指向 `006` 的问题仍存在，和本次 Linux durable file 实现无直接耦合，继续保留在 `common-risk-notes.md`。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T013 标记为完成。

## 是否更新 module-notes.md

- 是。更新了 `modules/store/io/module-notes.md`，同步 Linux durable file 的实现状态和测试边界。

## common-risk-notes.md 新增/删除/解决了哪些项

- 无新增项。
- 无删除项。
- 无已解决项。
