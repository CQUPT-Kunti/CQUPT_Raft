# Common Risk Notes

- 任务编号：T001
  问题：`.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 当前返回的 `FEATURE_DIR` 是 `specs/006-remove-kv-metadata-state-machine`，与本次执行的 `specs/007-storage-node-data-plane` 不一致。
  影响：继续依赖该脚本可能把实现流程、自动化检查或扩展 hook 引导到错误 feature 目录。
  建议后续在哪类任务处理：在后续 speckit 工作流或脚本修正类任务中校正 feature 选择逻辑，避免 007 任务执行时误绑定到 006。

- 任务编号：T009
  问题：`tests/support/store_test_utils.h` 中的 `MakeChecksumFixture()` 仍返回 `fixture-fnv1a:*` 形式，与当前 `storedemo::ComputeChunkChecksum()` 的 SHA-256 生产语义不一致。
  影响：后续 storage 集成测试如果继续复用该 helper，可能误把测试夹具摘要当成生产 checksum，导致用例语义漂移或断言失真。
  建议后续在哪类任务处理：在后续 storage 测试工具或 LocalDiskChunkStore 测试任务中统一切到生产 checksum helper，或显式区分 fixture checksum 与 production checksum。
