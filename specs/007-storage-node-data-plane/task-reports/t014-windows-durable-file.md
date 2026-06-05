# T014 Windows Durable File

## 修改文件

- `modules/store/io/durable_file.h`
- `modules/store/io/durable_file.cpp`
- `modules/store/io/module-notes.md`
- `modules/store/io/AGENTS.md`
- `tests/store_durable_file_test.cpp`
- `specs/007-storage-node-data-plane/tasks.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`

## 做了什么

- 为 `storedemo::DurableFile` 增加 `WindowsDurableFile`，在条件编译下接入 Windows file handle 写入、`FlushFileBuffers`、`MoveFileExW` publish、UTF-8 到 UTF-16 路径转换、long path 前缀处理和 Windows 错误码映射。
- Windows 路径校验补充了 absolute path、`..`、reserved names、非法字符和 root escape 拒绝逻辑。
- 将 Windows directory durability 明确建模为 explicit `kUnsupported`，避免返回 silent no-op success。
- 在 `tests/store_durable_file_test.cpp` 中补充 Windows 条件测试，用于后续 Windows 实机环境验证 flush/publish 成功路径、路径拒绝和 directory durability explicit unsupported 语义。
- 更新 `modules/store/io/module-notes.md` 和 `modules/store/io/AGENTS.md`，同步 Windows 实现状态、`.cpp` 对应函数和“未实机验证不能写成 PASS”的约束。
- 在 `tasks.md` 中将 T014 标记为完成，并新增 `T014-WIN` 作为后续 Windows 实机验证任务。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_durable_file" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 实机验证状态

- Windows 分支已实现。
- 当前环境无 Windows 编译/测试能力。
- Windows 实机验证待执行，已在 `tasks.md` 中新增 `T014-WIN`。

## 是否通过 T014

- 通过

## 是否可以进入 T015

- 可以进入 T015

## 当前任务发现的不合理点 / 警告 / 风险

- 当前只能确认 Linux 环境下条件编译接入没有破坏已有主线。
- `WindowsDurableFile::SyncDirectory(...)` 当前明确返回 `kUnsupported`，这是有意保守处理，不是遗漏。
- Windows 实机行为仍需通过 `T014-WIN` 验证，相关风险已记录到 `common-risk-notes.md`。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T014 标记为完成，并新增 `T014-WIN` 待验证任务，避免把未完成的 Windows 实机验证混进 T014 主结论。

## 是否更新 module-notes.md / AGENTS.md

- 是。更新了 `modules/store/io/module-notes.md` 和 `modules/store/io/AGENTS.md`，补充 Windows 公开类、`.cpp` 对应函数、directory durability 边界和 Windows 实机验证约束。

## common-risk-notes.md 新增/删除/解决了哪些项

- 新增 1 项：T014 Windows 实机验证缺失风险。
- 无删除项。
- 无已解决项。
