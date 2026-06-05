# T015 Path Normalization Layout

## 修改文件

- `modules/store/io/durable_file.h`
- `modules/store/io/durable_file.cpp`
- `modules/store/io/module-notes.md`
- `modules/store/io/AGENTS.md`
- `tests/store_durable_file_test.cpp`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `NormalizeDurableRelativePath(...)`，统一校验相对路径，拒绝绝对路径、`..`、Windows reserved names、非法字符和危险尾随空格/点。
- 新增 `ResolveDurablePathUnderRoot(...)`，把安全相对路径解析到 data root 内，保证最终路径不会逃出 root。
- 新增 `ChunkPathLayout` 和 `BuildChunkPathLayout(...)`，为后续 `LocalDiskChunkStore` 生成稳定的 final / staging 相对路径。
- 当前布局规则为：
  - final：`chunks/live/<shard-1>/<shard-2>/<chunk_id>.chunk`
  - staging：`chunks/staging/<shard-1>/<shard-2>/<chunk_id>.<staging_token>.tmp`
- `LinuxDurableFile::NormalizePath(...)` 和 `WindowsDurableFile::NormalizePath(...)` 改为复用统一 helper，避免平台间路径规则漂移。
- 扩展 `tests/store_durable_file_test.cpp`，覆盖合法路径、root 内约束、layout 生成、staging/final 区分、非法 chunk id、reserved names 和非法字符。
- 更新 `modules/store/io/module-notes.md` 和 `modules/store/io/AGENTS.md`，说明新增 helper 的职责和布局规则。
- 将 `tasks.md` 中 T015 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_durable_file" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 实机验证状态

- 当前环境无 Windows 编译/测试能力。
- T015 新增的路径 helper 主要通过平台无关测试固定规则，Windows 分支实机编译/运行仍沿用 `T014-WIN` 待验证任务。

## 是否通过 T015

- 通过

## 是否可以进入 T016

- 可以进入 T016

## 当前任务发现的不合理点 / 警告 / 风险

- 本任务未发现新的公共风险。
- 当前采用的是跨平台保守路径规则，会主动拒绝部分 Linux 本来可接受、但 Windows 不安全的路径形态，这是有意收敛而不是兼容性回退。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T015 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 是。更新了 `modules/store/io/module-notes.md` 和 `modules/store/io/AGENTS.md`，补充路径 normalization、chunk layout helper 和布局规则说明。

## common-risk-notes.md 新增/删除/解决了哪些项

- 无新增项。
- 无删除项。
- 无已解决项。
