# Common Risk Notes

- 任务编号：T001
  问题：`.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 当前返回的 `FEATURE_DIR` 是 `specs/006-remove-kv-metadata-state-machine`，与本次执行的 `specs/007-storage-node-data-plane` 不一致。
  影响：继续依赖该脚本可能把实现流程、自动化检查或扩展 hook 引导到错误 feature 目录。
  建议后续在哪类任务处理：在后续 speckit 工作流或脚本修正类任务中校正 feature 选择逻辑，避免 007 任务执行时误绑定到 006。

- 任务编号：T005
  问题：工作区当前存在 `.codegraph/codegraph.db`、`.codegraph/codegraph.db-shm`、`.codegraph/codegraph.db-wal` 修改状态，即使本任务未使用 CodeGraph，也可能混入业务提交。
  影响：后续提交 007 任务时可能误带本地 CodeGraph 索引副产物，增加无关噪音和合并干扰。
  建议后续在哪类任务处理：在提交前检查并排除 `.codegraph/` 下索引副产物；除非任务明确要求，不应提交 CodeGraph 数据库相关文件。
