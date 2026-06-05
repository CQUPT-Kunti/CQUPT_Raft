# T019 Bounded Executor

## 修改文件

- `modules/store/runtime/storage_executor.h`
- `modules/store/runtime/storage_executor.cpp`
- `modules/store/runtime/module-notes.md`
- `modules/store/runtime/AGENTS.md`
- `modules/store/AGENTS.md`
- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `modules/store/runtime` 模块，定义 `BoundedStorageExecutor` 及其有界任务队列。
- 新增执行器配置、提交结果、停机请求/结果、统计快照等基础类型。
- 实现固定 worker 数、固定队列容量、非阻塞 `Submit(...)` 和 overloaded / stopped / invalid_argument 返回语义。
- 实现 `Shutdown(kDrain)` 与 `Shutdown(kCancelPending)` 两种停机模式。
- worker 执行路径增加异常捕获和失败统计，避免单个任务异常直接导致 worker 线程静默死亡。
- 为 timeout / cancellation 预留 `StorageTaskContext` 扩展点，但不提前实现复杂取消传播。
- 将新模块接入 `raft_core` 构建。
- 更新 `modules/store/AGENTS.md`，把 `runtime/` 纳入 store 子模块索引。
- 将 `tasks.md` 中 T019 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index" --output-on-failure`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只使用标准 C++ 线程、mutex、condition_variable 等平台无关原语。
- 因此本任务不需要新增 `T019-WIN`。

## 是否通过 T019

- 通过

## 是否可以进入 T020

- 可以进入 T020

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 timeout / cancellation 仍是接口扩展点，不是已经生效的运行中任务取消能力。
- `Shutdown()` 当前按 owner 线程模型设计，不支持在 executor worker 回调内部自停或自析构。
- T018 的 chunk guard 风险与本任务无关，仍需保留，不能误删。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T019 标记为完成。

## 是否新增 module-notes.md / AGENTS.md

- 新增 `modules/store/runtime/module-notes.md`
- 新增 `modules/store/runtime/AGENTS.md`
- 同时更新了 `modules/store/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 7 项风险：
  - T001 `.specify` feature-dir 误指向 `006`
  - T009 checksum fixture 与生产 SHA-256 语义不一致
  - T014 Windows durable file 缺少实机验证
  - T016 并发修改下的稳定分页风险
  - T018 后续业务入口必须显式持有 chunk guard
  - T019 timeout / cancellation 仅为扩展点
  - T019 shutdown 的 owner-thread 使用边界

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 2 项：两条 T019 风险，分别记录 timeout/cancellation 仍是扩展点，以及 `Shutdown()` 的 owner-thread 边界。
- 删除 0 项。
- 解决 0 项。
- 保留 5 项已有风险：T001、T009、T014、T016、T018。
