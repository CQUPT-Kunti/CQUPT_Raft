# Common Risk Notes

- 任务编号：T001
  问题：`specs/007-storage-node-data-plane/plan.md` 与 `tasks.md` 仍以 `modules/raft/storage_node/` 作为 StorageNode 代码路径，但当前实现约束已经切换为 `modules/store/`。
  影响：后续任务如果继续照旧文档落地，可能出现 `modules/store/` 与 `modules/raft/storage_node/` 双路径并存、CMake 重复接线、no-KV 审计范围不一致。
  建议后续在哪类任务处理：在后续最早涉及 007 文档/任务对齐的 setup 或 polish 类任务中统一修正路径说明，再进入更深的数据面实现。

- 任务编号：T001
  问题：`.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 当前返回的 `FEATURE_DIR` 是 `specs/006-remove-kv-metadata-state-machine`，与本次执行的 `specs/007-storage-node-data-plane` 不一致。
  影响：继续依赖该脚本可能把实现流程、自动化检查或扩展 hook 引导到错误 feature 目录。
  建议后续在哪类任务处理：在后续 speckit 工作流或脚本修正类任务中校正 feature 选择逻辑，避免 007 任务执行时误绑定到 006。
