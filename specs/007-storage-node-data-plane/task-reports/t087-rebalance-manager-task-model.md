# T087 RebalanceManager Task Model

## 修改文件

- `modules/store/maintenance/rebalance_manager.h`
- `modules/store/maintenance/rebalance_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `CMakeLists.txt`
- `tests/storage_rebalance_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t087-rebalance-manager-task-model.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增生产 `RebalanceManager` task model，支持创建 capacity imbalance、hotspot、new node join、draining、maintenance 五类 rebalance task。
- 固定 task 字段：`task_id`、`chunk_id`、`source_node`、`target_node`、`reason`、`expected_checksum`、`expected_size`、`state`、`progress`、`attempts`、`last_error`、`last_error_detail`，并补上 `submitted/started/completed/retry_after` 时间事实。
- 固定 submit / lookup / list / mark running / update progress / complete / fail / cancel / retry 语义。
- submit 阶段接入最小 source/target 校验：chunk identity、expected checksum/size、`source != target`、source chunk state、registry snapshot 中的 source live/healthy、target live/healthy/not overloaded/capacity sufficient。
- 扩展 `storage_rebalance` 测试，固定 reason 创建、duplicate/already_exists、queue/task overload、invalid source/target/facts、生命周期状态迁移、稳定排序和默认不自动 copy 边界。

## RebalanceManager task model 输入、输出和状态语义

- 输入：`RebalanceTaskRequest`，包含 chunk identity、source/target node、reason、expected checksum/size、source chunk state 和 task context。
- 输出：`RebalanceManagerSubmitResult` / `RebalanceTaskOperationResult` / `RebalanceManagerStats`。
- 状态语义：`Queued`、`Running`、`Completed`、`Failed`、`Cancelled`、`RetryPending`。

## capacity / hotspot / new node join task 当前语义

- 三类 task 都可创建；本次也一并支持 `Draining` / `Maintenance`。
- reason 会写入稳定 `task_id`，同一 chunk/source/target 但不同 reason 会得到不同 task_id。
- 当前 reason 只表达 rebalance 触发原因，不驱动自动 copy 或后台调度。

## progress / attempts / last_error 当前边界

- `MarkTaskRunning()` 才会增加 `attempts`。
- `UpdateTaskProgress()` 只允许在 `Running` 状态更新。
- `CompleteTask()` 只把 task model 标成完成，不代表 copy、target durable、manifest update 或 source cleanup 已发生。
- `FailTask()` / `CancelTask()` 只更新 task model 和错误事实。

## 与 T088 copy/verify/manifest coordination 的边界

- 当前不做 rebalance copy / verify。
- 当前不做 metadata manifest coordination。
- 当前不做 source cleanup。
- 当前不做自动后台调度。
- 当前不做 rebalance task persistence。

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata / Raft。
- 不保存 object payload。

## 是否使用 tests/test_file/test_file.zip

- 未使用。

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
- `ctest --test-dir build/linux -R "storage_rebalance|rebalance_manager|storage_scrub_repair" --output-on-failure 2>&1 | tee tmp/007/t087-rebalance-manager-task-model.log`
- 实际命中测试：`storage_scrub_repair`、`storage_rebalance`
- 结果：PASS
- 日志：`tmp/007/t087-rebalance-manager-task-model.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次记录以实际验证结果为准；若失败再补。

## Windows 验证判断

- T087 是平台无关 rebalance task model。
- 当前无 Windows 编译环境，本任务不宣称 Windows PASS。
- 因为本次不涉及真实 copy / durable write / delete，Windows 文件语义风险继续保留到后续 T088 及相关实机验证任务。

## 是否通过 T087

- 是。

## 是否可以进入 T088

- 可以。

## 当前任务发现的不合理点 / 警告 / 风险

- `specs/007-storage-node-data-plane/gc-repair-rebalance-contract.md` 当前仓库中不存在，本次按 `tasks.md`、既有任务报告、`storage_rebalance_test.cpp` 和 `common-risk-notes.md` 继续实现。
- submit 阶段对 source/target 的可用性判断只基于单次 registry snapshot 与请求携带的 source chunk state；copy/manifest/source cleanup 的新鲜度时间窗仍待 T088 继续收口。

## 是否更新 module-notes.md / AGENTS.md

- 已更新 `modules/store/maintenance/module-notes.md`。
- 未更新 `AGENTS.md`。

## 是否修改高频文档及原因

- 已修改 `specs/007-storage-node-data-plane/tasks.md`，把 T087 标记为完成并记录实际修改与验收。
- 已修改 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`，保留并补充 T087 后续风险。

## common-risk-notes.md 读取结果

- 已读取并保留既有 T080/T086 及更早任务的未关闭风险。

## common-risk-notes.md 新增/删除/保留情况

- 新增：T087 task model 已实现，但 rebalance copy/verify、metadata manifest coordination、source cleanup、自动调度、持久化和 source/target 新鲜度时间窗仍未实现。
- 删除：无。
- 保留：T080、T086 以及 RepairManager persistence、read-side repair、Windows 实机验证、metadata/registry facts 新鲜度、half-migrated manifest 等既有风险。
