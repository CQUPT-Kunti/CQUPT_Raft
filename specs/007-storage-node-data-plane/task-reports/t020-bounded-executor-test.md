# T020 Bounded Executor Test

## 修改文件

- `tests/store_executor_test.cpp`
- `tests/CMakeLists.txt`
- `modules/store/runtime/module-notes.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/store_executor_test.cpp`，为 `BoundedStorageExecutor` 补充单元测试，覆盖：
  - 非法配置修正
  - 空任务提交返回 `kInvalidArgument`
  - 队列容量上限
  - 队列满返回 overloaded
  - `Shutdown(kDrain)` 会执行完已提交任务
  - `Shutdown(kCancelPending)` 会丢弃尚未开始的 pending task
  - shutdown 后不再接受新任务
  - worker 捕获任务异常后仍可继续执行后续任务
  - 当前 timeout/cancellation 只是扩展点，不会自动取消任务
  - worker 内调用 `Shutdown()` 返回明确边界错误
  - 析构路径会按 drain 语义回收 worker 线程
- 在 `tests/CMakeLists.txt` 注册 `store_executor`，接入 `storage-node` 和 `platform-neutral` 标签。
- 更新 `modules/store/runtime/module-notes.md`，把 T020 已固定的执行器语义补充进去。
- 维护 `common-risk-notes.md`，更新两条 T019 风险描述：语义已被测试固定，但边界仍需后续任务遵守。
- 将 `tasks.md` 中 T020 标记为完成。

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "store_executor" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -R "store_types|store_durable_file|store_chunk_index" --output-on-failure`
  - PASS
- `ctest --test-dir build/linux -N -L storage-node`
  - PASS，包含 `store_types`、`store_durable_file`、`store_chunk_index`、`store_executor`
- `ctest --test-dir build/linux -N -L platform-neutral`
  - PASS，`store_executor` 已注册到 `platform-neutral`
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS

## Windows 验证判断

- 本任务只测试标准 C++ 线程、mutex、condition_variable、future/promise 等平台无关语义。
- 因此本任务不需要新增 `T020-WIN`。

## 是否通过 T020

- 通过

## 是否可以进入 T021

- 可以进入 T021

## 当前任务发现的不合理点 / 警告 / 风险

- timeout / cancellation 的当前语义已经被测试固定为“仅承载上下文，不自动取消运行中任务”，但真正的取消传播仍未实现。
- `Shutdown()` 的 owner-thread 约束已经被测试固定，但这条使用边界在后续接入任务里仍然必须遵守。
- T018 的 chunk guard 风险与本任务无关，仍需保留。

## 是否修正了高频文档，为什么

- 是。修改了 `specs/007-storage-node-data-plane/tasks.md`，将 T020 标记为完成。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/runtime/module-notes.md`
- 未修改 `modules/store/runtime/AGENTS.md`

## common-risk-notes.md 读取结果

- 读取到 7 项风险：
  - T001 `.specify` feature-dir 误指向 `006`
  - T009 checksum fixture 与生产 SHA-256 语义不一致
  - T014 Windows durable file 缺少实机验证
  - T016 并发修改下的稳定分页风险
  - T018 后续业务入口必须显式持有 chunk guard
  - T019 timeout / cancellation 仍未实现真正取消传播
  - T019 shutdown 的 owner-thread 使用边界

## common-risk-notes.md 新增/删除/保留/解决情况

- 新增 0 项。
- 删除 0 项。
- 解决 0 项。
- 保留 7 项已有风险。
- 其中两条 T019 风险已更新描述：T020 已补测试固定当前语义，但它们仍然是后续接入阶段必须显式处理的边界。
