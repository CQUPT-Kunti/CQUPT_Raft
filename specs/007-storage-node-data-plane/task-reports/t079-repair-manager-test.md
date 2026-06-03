# T079 RepairManager Test

## 修改文件

- `tests/storage_scrub_repair_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t079-repair-manager-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 在既有 `storage_scrub_repair` 测试中新增 test-only `TestOnlyRepairRunner` 与 `RepairReplicaFactsLedger`。
- `TestOnlyRepairRunner` 复用 T078 的 `ScrubRepairCandidate` 作为输入，使用真实 `LocalDiskChunkStore` 校验 source checksum / 读取 payload，使用真实 `PlacementManager + StorageNodeRegistry` 选择 target，并在 target durable success 后才更新 test-only replica facts。
- 通过 test-only `RecordingDurableFile` 注入 target durable failure，固定“durable 失败时不得更新 facts”的 contract。

## RepairManager contract 覆盖场景

- under-replicated chunk 选择 healthy source + healthy target
- corrupted / quarantined / stale / unavailable / unhealthy source 不能作为 repair source
- unhealthy / overloaded / disk pressure high/full / capacity 不足 / stale target 不能作为 repair target
- source checksum mismatch 时 repair 失败，不写 target facts
- target durable 成功后，才更新 test-only replica facts
- target durable 失败时，不更新 test-only replica facts
- target 已存在同 checksum chunk 时 idempotent success
- 所有 source 不可用时，返回明确 failure
- 所有 target 不可用时，返回明确 failure
- repeated repair 幂等，不产生重复 target facts
- repair 不修改 metadata manifest
- repair 不调用 Raft

## source / target selection 当前语义

- source selection：
  - 输入来自 T078 的 `ScrubRepairCandidate.healthy_source_replicas`
  - 运行 repair 时再次用真实 `StorageNodeRegistry` snapshot 校验：
    - liveness 必须是 `Live`
    - health 必须是 `Healthy`
  - 再用真实 `LocalDiskChunkStore::ReadChunk(expected_checksum, verify_checksum=true)` 验证 source payload
  - checksum mismatch / quarantine / unavailable / stale / unhealthy source 都会被跳过
- target selection：
  - 使用真实 `PlacementManager::SelectPlacement(...)`
  - `excluded_nodes` 使用当前 manifest replica_nodes
  - 目标节点必须通过 registry facts 的 liveness / health / write overload / disk pressure / capacity 过滤

## target durable before facts update 当前边界

- target write 成功路径：
  - 先执行真实 `LocalDiskChunkStore::WriteChunk()`
  - 只有 `WriteChunk()` 返回 durable success / already_exists success 后，才调用 test-only `RepairReplicaFactsLedger::MarkDurable(...)`
- target write 失败路径：
  - `WriteChunk()` 返回失败后立即退出
  - 不更新 `RepairReplicaFactsLedger`
  - 不把失败伪装成 repair success

## test-only repair helper 与生产 RepairManager 当前边界

- `TestOnlyRepairRunner` / `RepairReplicaFactsLedger` 只存在于 `tests/storage_scrub_repair_test.cpp`
- 它们不是生产 `RepairManager`
- 不新增：
  - `modules/store/maintenance/repair_manager.*`
  - `RepairChunk` proto
  - `RepairChunk` RPC/service/client
  - repair task persistence
  - read-side repair
- 当前 helper 只固定 contract，不代表生产后台 repair 编排已完成

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不修改 metadata manifest
- 不把 object payload 写入 metadata / Raft / snapshot
- repair copy 的 payload 只在 test-only repair runner 内通过真实 `ReadChunk -> WriteChunk` 流转，不进入 metadata / Raft

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_scrub_repair|repair_manager|scrub_repair" --output-on-failure 2>&1 | tee tmp/007/t079-repair-manager.log`
  - PASS
  - 实际匹配到的测试名为 `storage_scrub_repair`
  - 日志路径：`tmp/007/t079-repair-manager.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T079 是平台无关 repair contract 测试
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮测试涉及真实 chunk copy / durable write / quarantine / publish failure injection，Windows copy / rename / durability / sharing 语义仍待后续实机验证

## 是否通过 T079

- 是

## 是否可以进入 T080

- 可以
- T080 应继续做 RebalanceManager 测试骨架，不要把本轮 contract 测试扩展成生产 RepairManager / RepairChunk 实现

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 repair source / target 选择仍依赖 registry snapshot 的新鲜度和 manifest replica_nodes 的快照新鲜度，尚未定义生产级 snapshot boundary。
- 当前 target durable success 之后只更新 test-only replica facts，没有定义生产级 repair facts persistence / task progress / retry state。
- 当前 `PlacementManager` 的 target 选择只基于现有 registry facts；registry failure cache / recent failure scoring 仍未实现。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只扩展测试和文档，没有修改生产模块实现

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T079 完成并记录真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T079 风险条目，保留 source/target 选择新鲜度、target durable 后 facts 更新顺序等未解风险

## common-risk-notes.md 读取结果

- 已读取
- 未删除生产 RepairManager、RepairChunk RPC、repair task persistence、read-side repair、RebalanceManager、Windows 实机验证等既有风险
- prerequisites 脚本误指向 006 的问题继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T079`，记录 repair source / target 选择与 target durable 后 facts 更新顺序仍依赖 registry / manifest 新鲜度和生产级 persistence 语义
- 删除：
  - 无
- 保留：
  - 生产 RepairManager、RepairChunk RPC、repair task persistence、read-side repair、RebalanceManager、Windows 实机验证等后续风险继续保留
