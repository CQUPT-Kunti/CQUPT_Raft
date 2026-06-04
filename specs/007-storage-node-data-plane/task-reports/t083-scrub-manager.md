# T083 ScrubManager

## 修改文件

- `modules/store/maintenance/scrub_manager.h`
- `modules/store/maintenance/scrub_manager.cpp`
- `modules/store/maintenance/module-notes.md`
- `CMakeLists.txt`
- `tests/storage_scrub_repair_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t083-scrub-manager.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增生产 `ScrubManager`，复用 `BoundedStorageExecutor` 提供 bounded background queue。
- 新增 scrub task model、submit/drain/stop/query/stats 语义，以及 task 状态、attempts、last_error、result facts 可观察面。
- 默认 scrub 执行路径复用真实 `ChunkStore::StatChunk()`、`StorageNodeRegistry::Snapshot()` 和 `ReplicaPolicySelector::SelectReadReplicas()`，输出 corrupted / lost / under-replicated facts 与 repair candidate。
- 扩展 `storage_scrub_repair` 测试，覆盖 submit、queue full、stop/drain、healthy/corrupted/lost/under-replicated、source 过滤、重复 scrub 稳定性、失败 task 记账，以及 foreground IO 不经过 scrub queue 的 contract。

## ScrubManager 输入、输出和 task 状态语义

- 输入：
  - `ScrubTask`：`task_id`、`ScrubManifest`、`StorageTaskContext`
  - `ScrubManifest`：`chunk_id/object identity`、expected checksum/size、replica nodes、desired replica count
- 输出：
  - `ScrubTaskResult`：`replica_facts` + 可选 `repair_candidate`
  - `ScrubReplicaFact`：node_id、status、state_before/state_after、observed checksum/size、known_corrupted、known_missing、quarantined、checksum_verified
  - `ScrubRepairCandidate`：`chunk_id`、expected checksum/size、bad replicas、healthy source replicas、under_replicated、lost_or_unrecoverable
- task 状态：
  - `Queued`
  - `Running`
  - `Completed`
  - `Failed`
  - `Cancelled`

## bounded queue / low-priority IO 当前边界

- bounded queue 通过独立 `ScrubManager -> BoundedStorageExecutor` 后台通道实现。
- queue 满时返回 `Overloaded`，stop 后拒绝新任务。
- `Drain()` 只等待当前已提交的 queued/running task 收口。
- `CancelPending` 只取消尚未开始的 queued task；运行中 task 不做强制中断。
- 当前 low-priority 语义固定为“foreground `ReadChunk/WriteChunk` 不经过 scrub queue、后台队列有界、不无界堆积”，不实现 OS-level IO priority。

## corrupted / lost / under-replicated facts 输出语义

- corrupted：
  - checksum mismatch 或已 quarantined/corrupted 副本会被标成 `known_corrupted`
  - 如本地 verify 触发 T072 quarantine，则 `quarantined=true`
- lost：
  - bad replicas 存在且没有任何 healthy source 时，产出 `lost_or_unrecoverable=true` candidate
- under-replicated：
  - healthy source 数量小于 `desired_replica_count` 时，产出 `under_replicated=true` candidate
- stale / unavailable / unhealthy / high-disk-pressure 副本不会进入 `healthy_source_replicas`

## repair candidate 当前边界

- `ScrubManager` 只发现问题并产出 repair candidate。
- 不实现 `RepairManager`、`RepairChunk` copy flow、read-side repair 或 rebalance。
- 当前 candidate 仍依赖实时 registry snapshot、本地 quarantine/missing 事实和调用方提供的 manifest，没有 task persistence、failure cache 或 metadata freshness 协议。

## 是否调用 metadata / Raft；是否保存 payload

- 否
- `ScrubManager` 不修改 metadata manifest，不调用 Raft，不把 object payload 写入 metadata / Raft

## 是否使用 tests/test_file/test_file.zip

- 否
- T083 新增/扩展测试 payload 全部使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_scrub_repair|scrub_manager|storage_node_service|storage_node_client" --output-on-failure 2>&1 | tee tmp/007/t083-scrub-manager.log`
  - PASS
- 日志路径：
  - `tmp/007/t083-scrub-manager.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T083 是平台无关后台队列和 scrub manager 任务。
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS。
- 因为 scrub 仍涉及真实坏块、quarantine 和文件状态转换，Windows quarantine / rename / runtime 文件语义继续保留为待验证风险。

## 是否通过 T083

- 是

## 是否可以进入 T084

- 可以
- 当前已补齐生产 `ScrubManager` bounded queue 和发现链路；下一步应进入 `RepairManager` task model，不要把本轮结果误读成 repair flow 已完成。

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 `ScrubManager` 仍是进程内 `ChunkStore*` + registry snapshot 编排，不包含跨节点 `ScrubChunk` fan-out。
- `CancelPending` 只覆盖未开始任务，运行中任务仍按 natural completion 收口。
- repair candidate 与 metadata manifest / registry facts 的 freshness 协议、failure cache 和 task persistence 仍未落地。

## 是否更新 module-notes.md / AGENTS.md

- 更新了 `modules/store/maintenance/module-notes.md`
  - 记录 `ScrubManager` 职责、bounded queue、submit/drain/stop 边界和关键 helper
- 未更新 `AGENTS.md`

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 标记 T083 完成
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 新增 T083 风险，保留 repair/rebalance/read-side repair/Windows 等未解项

## common-risk-notes.md 读取结果

- 已读取
- 保留了 `RepairManager` 未实现、`RepairChunk` copy flow 未实现、`RebalanceManager` 未实现、repair task persistence 未实现、read-side repair 未实现、Windows 实机验证待完成、metadata/registry freshness 风险，以及 `.specify/scripts/bash/check-prerequisites.sh` 仍误指向 006 的风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T083`，记录生产 `ScrubManager` 已有 bounded queue 与 task state，但仍缺 task persistence、failure cache、manifest/registry freshness 和 Windows runtime 验证
- 删除：
  - 无
- 保留：
  - `RepairManager`、`RepairChunk` copy flow、`RebalanceManager`、repair task persistence、read-side repair、Windows 实机验证、metadata/registry freshness 等后续风险继续保留
