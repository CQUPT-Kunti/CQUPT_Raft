# Common Risk Notes

- 任务编号：T001
  问题：`.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 当前返回的 `FEATURE_DIR` 是 `specs/006-remove-kv-metadata-state-machine`，与本次执行的 `specs/007-storage-node-data-plane` 不一致。
  影响：继续依赖该脚本可能把实现流程、自动化检查或扩展 hook 引导到错误 feature 目录。
  建议后续在哪类任务处理：在后续 speckit 工作流或脚本修正类任务中校正 feature 选择逻辑，避免 007 任务执行时误绑定到 006。

- 任务编号：T002
  问题：当前 `tests/CMakeLists.txt` 中多处 `gtest_discover_tests(... PROPERTIES LABELS "...;...")` 写法在本地实际只暴露首个标签。
  影响：后续若依赖多标签做 `ctest -L` 分组，可能出现 `platform-neutral-fallback`、`durability-boundary`、`storage-node-concurrency` 等标签不可见或筛选不全。
  建议后续在哪类任务处理：在后续测试基础设施或 storage CTest 分组任务中统一修正 helper/注册模式，并回归验证现有标签集合。
