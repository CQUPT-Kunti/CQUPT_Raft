# T086 Under-Replicated Repair

## 修改文件

- `modules/store/maintenance/scrub_manager.h`
- `modules/store/maintenance/scrub_manager.cpp`
- `modules/store/maintenance/repair_manager.h`
- `modules/store/maintenance/repair_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `tests/storage_scrub_repair_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t086-under-replicated-repair.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 给生产 `ScrubManager` 的 `ScrubRepairCandidate` 补上 healthy replica count / required replica count / missing replica count。
- 把 healthy replica 判定收口为 checksum verified、非 corrupted/missing、registry liveness live、health healthy、disk pressure 非 high/full。
- 新增 `RepairManager::SubmitUnderReplicatedTask()`，允许把 completed scrub task 中的 under-replicated fact 直接转换成 repair task。
- 扩展 `storage_scrub_repair`，覆盖 under-replicated 计数、source/target 选择、重复扫描幂等、lost/no-target 边界，以及 repair task 交给 T085 `RunTask()` 后的 durable copy。

## under-replicated detection 输入、输出和状态语义

- 输入：
  - `ScrubManifest`
  - 各 replica 的 checksum / size / state 事实
  - `StorageNodeRegistry` snapshot
  - `desired_replica_count`
- 输出：
  - `ScrubRepairCandidate`
  - `healthy_replica_count`
  - `required_replica_count`
  - `missing_replica_count`
  - `under_replicated`
  - `lost_or_unrecoverable`
- 状态语义：
  - `healthy_replica_count < required_replica_count` 时记为 under-replicated
  - 没有 healthy source 时记为 `lost_or_unrecoverable`
  - `RepairManager::SubmitUnderReplicatedTask()` 会把 under-replicated fact 显式映射为 accepted / already_exists / overloaded / invalid / lost_or_unrecoverable / no_healthy_source / no_healthy_target

## healthy replica 计数语义

- 计入 healthy replica 的条件：
  - checksum verified
  - 非 corrupted
  - 非 missing
  - registry liveness 为 live
  - node health 为 healthy
  - disk pressure 不是 high/full
- 不计入 healthy replica：
  - corrupted
  - quarantined
  - missing
  - stale
  - unavailable
  - unhealthy / degraded / draining

## source / target selection 语义

- source：
  - 从 scrub candidate 的 `healthy_source_replicas` 按稳定顺序选第一个当前仍 live + healthy 的 source
  - 运行前仍会再过一次 registry snapshot revalidate
- target：
  - 复用现有 `PlacementManager`
  - 排除 manifest 中已有 replica nodes
  - 必须 live、可写、非 overloaded、容量足够
  - 若 target 已存在同 checksum/size chunk，后续 T085 copy flow 会走幂等 success

## repair task 生成与 T085 copy flow 边界

- `SubmitUnderReplicatedTask()` 只做 under-replicated fact -> repair task 的规划和入队。
- 真正的 source read / checksum verify / target durable write 仍由 T085 的 `RepairManager::RunTask()` 执行。
- task `Completed` 只表示 target data-plane durable complete，不代表 metadata manifest 已更新。

## metadata manifest coordination 当前边界

- 本任务不实现 metadata manifest coordination。
- 不更新 metadata manifest。
- 不做 source cleanup。
- 不做 replica facts publish。

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot / state machine
- payload 只在 T085 repair copy flow 中流经 source/target data-plane

## 是否使用 tests/test_file/test_file.zip

- 否
- 测试 payload 全部使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
  - 日志路径：`tmp/007/t086-build.log`
- `ctest --test-dir build/linux -R "storage_scrub_repair|under_replicated|repair_manager|scrub_manager" --output-on-failure 2>&1 | tee tmp/007/t086-under-replicated-repair.log`
  - PASS
  - 实际匹配到的测试名：
    - `storage_scrub_repair`
  - 日志路径：`tmp/007/t086-under-replicated-repair.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T086 是平台无关 under-replicated detection / repair task 生成任务。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- 因为测试仍覆盖真实 target durable write / file copy 语义，Windows 文件 copy / rename / durable 语义继续保留为待验证风险。

## 是否通过 T086

- 是

## 是否可以进入 T087

- 可以
- T087 应继续做 RebalanceManager task model，不要把本轮结果扩展成 metadata manifest coordination、read-side repair 或 rebalance copy。

## 当前任务发现的不合理点 / 警告 / 风险

- under-replicated detection 仍依赖 scrub manifest 和 registry snapshot 的运行时新鲜度。
- repair task 幂等 key 仍取决于当前 source/target 规划，如果 manifest / registry facts 漂移，后续可能生成不同 task plan。
- target durable 之后仍没有 metadata manifest coordination、task persistence 或 source cleanup 协议。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/maintenance/module-notes.md`
- 未更新 `AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T086 完成并记录真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T086 风险条目，保留 metadata manifest coordination、task persistence、read-side repair、rebalance 和 Windows 实机验证等未解风险

## common-risk-notes.md 读取结果

- 已读取
- 保留了 metadata manifest coordination 未实现、RepairManager persistence 未实现、read-side repair 未实现、RebalanceManager 未实现、Windows 实机验证待完成、metadata / registry facts 新鲜度风险，以及 repair/rebalance 后 manifest 更新与 source cleanup 一致性风险
- `.specify/scripts/bash/check-prerequisites.sh` 误指向 006 的风险继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T086`，记录 under-replicated detection 仍依赖 manifest/registry freshness、repair task 幂等 key 仍有时间窗、target durable 后仍缺 metadata coordination / persistence / cleanup 协议
- 删除：
  - 无
- 保留：
  - metadata manifest coordination、RepairManager persistence、read-side repair、RebalanceManager、Windows 实机验证、metadata / registry facts 新鲜度、repair/rebalance 后 source cleanup 一致性等后续风险继续保留
