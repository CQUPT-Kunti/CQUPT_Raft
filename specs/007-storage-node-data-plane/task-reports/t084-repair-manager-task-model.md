# T084 RepairManager Task Model

## 修改文件

- `modules/store/maintenance/repair_manager.h`
- `modules/store/maintenance/repair_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `modules/store/maintenance/scrub_manager.h`
- `CMakeLists.txt`
- `tests/storage_scrub_repair_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t084-repair-manager-task-model.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增生产 `RepairManager` task model。
- 支持从 `ScrubManifest + ScrubRepairCandidate` 创建 repair task，并在 submit 阶段固定 source_node / target_node。
- 新增 repair task 的 submit / lookup / list / cancel / retry / running / progress / complete / fail 生命周期接口。
- 扩展 `storage_scrub_repair` 测试，覆盖 task 创建、source/target/expected facts 记录、duplicate already_exists、queue/task capacity overload、invalid source/target/缺失字段、cancel / retry / fail / complete / list / lookup 边界。
- 顺手修正 `ScrubManager` / `RepairManager` 成员声明顺序，使 `config_` 在 `impl_` 前构造，避免未初始化配置被 `impl_` 读取。

## RepairManager task model 输入、输出和状态语义

- 输入：
  - `RepairTaskRequest`
  - `ScrubManifest`
  - `ScrubRepairCandidate`
  - `StorageTaskContext`
- 输出：
  - `RepairTask`
  - `RepairManagerSubmitResult`
  - `RepairTaskOperationResult`
  - `RepairManagerStats`
- task 状态：
  - `Queued`
  - `Running`
  - `Completed`
  - `Failed`
  - `Cancelled`
  - `RetryPending`

## source_node / target_node / expected checksum / size 记录语义

- `source_node`
  - 从 `repair_candidate.healthy_source_replicas` 中，结合真实 registry snapshot 选择第一个 live + healthy + 非 high/full disk pressure source。
- `target_node`
  - 使用真实 `PlacementManager + StorageNodeRegistry`，以 `manifest.replica_nodes` 为 excluded_nodes 规划 1 个 target。
- `expected_checksum / expected_size`
  - 来自 `repair_candidate` 与 `manifest` 的一致性事实，submit 时必须存在且一致，否则直接拒绝。
- `chunk_id`
  - 由 `manifest.identity.chunk_id` 与 `repair_candidate.chunk_id` 收口并校验一致。
- `task_id`
  - 由 `chunk_id + expected checksum + expected size + source_node + target_node` 生成稳定字符串，同一规划重复提交返回 `already_exists`。

## progress / attempts / last_error 当前边界

- `progress_percent`
  - 初始为 `0`
  - `MarkTaskRunning()` 至少推进到 `1`
  - `UpdateTaskProgress()` 只允许在 running 状态下更新
  - `CompleteTask()` 收口到 `100`
- `attempts`
  - 只在 `MarkTaskRunning()` 时增加
  - retry 不直接增加 attempts，直到下一次重新进入 running
- `last_error / last_error_detail`
  - `FailTask()` 记录失败事实
  - `CancelTask()` 记录 cancelled
  - `CompleteTask()` 清空错误
- `RetryPending`
  - 表示 task model 进入“等待下一次 repair attempt”的状态，不代表 copy 已重新发起

## 与 T085 RepairChunk copy flow 的边界

- `RepairManager` 当前只做 task model 和 source/target 规划。
- 不执行 `RepairChunk` copy flow。
- 不做 target durable write。
- 不校验 target 写后 checksum。
- 不更新 metadata manifest。
- 不做 repair facts publish / persistence。
- 不做 read-side repair。

## 是否调用 metadata / Raft；是否保存 payload

- 否
- `RepairManager` 不调用 metadata，不调用 Raft，不保存 payload，不把 object payload 写入 metadata / Raft

## 是否使用 tests/test_file/test_file.zip

- 否
- T084 新增/扩展测试 payload 全部使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_scrub_repair|repair_manager|scrub_manager" --output-on-failure 2>&1 | tee tmp/007/t084-repair-manager-task-model.log`
  - PASS
- 实际匹配到的测试名：
  - `storage_scrub_repair`
- 日志路径：
  - `tmp/007/t084-repair-manager-task-model.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T084 是平台无关 repair task model。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- 本轮默认不涉及真实 copy / durable write / rename；Windows repair runtime、copy、durability 和 manifest coordination 继续保留待验证风险。

## 是否通过 T084

- 是

## 是否可以进入 T085

- 可以
- T085 应继续做 `RepairChunk` copy flow，不要把本轮 task model 误读成 repair 执行链路已完成。

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 `RepairManager` 只在 submit 时规划 source/target，没有 task persistence、restart resume 或 source/target revalidation。
- `RetryPending` 只是 task model 状态，不会自动发起 copy。
- source/target 仍依赖 registry facts 和 placement snapshot 新鲜度，没有 failure cache 或 metadata manifest coordination。
- `ScrubManager` / `RepairManager` 都存在“成员声明顺序必须先构造 config 再构造 impl”这一类 C++ 初始化顺序坑，本轮已做最小修正。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/maintenance/module-notes.md`
  - 记录 `RepairManager` 职责、task 字段、submit / lookup / list / cancel / retry / complete / fail / progress 语义，以及与 T085 的边界
- 未更新 `AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 标记 T084 完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 新增 T084 风险，保留 RepairChunk copy flow、under-replicated detection、task persistence、metadata coordination、Windows 等未解项

## common-risk-notes.md 读取结果

- 已读取
- 保留了 `RepairChunk` copy flow 未实现、under-replicated detection 未实现、RepairManager 持久化未实现、read-side repair 未实现、RebalanceManager 未实现、metadata manifest coordination 未实现、Windows 实机验证待完成，以及 metadata / registry facts 新鲜度风险
- `.specify/scripts/bash/check-prerequisites.sh` 误指向 006 的风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T084`，记录 RepairManager 当前只有 task model 和 source/target 规划，没有 copy、durable、manifest coordination、task persistence 或 failure cache
- 删除：
  - 无
- 保留：
  - `RepairChunk` copy flow、under-replicated detection、RepairManager 持久化、read-side repair、RebalanceManager、metadata manifest coordination、Windows 实机验证、metadata/registry freshness 等后续风险继续保留
