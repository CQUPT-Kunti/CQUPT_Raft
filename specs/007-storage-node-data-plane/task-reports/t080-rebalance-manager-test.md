# T080 RebalanceManager Test

## 修改文件

- `tests/storage_rebalance_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t080-rebalance-manager-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增独立 `storage_rebalance` contract 测试文件，避免继续膨胀 `storage_scrub_repair_test.cpp`。
- 在测试内实现 test-only：
  - `TestOnlyRebalanceRunner`
  - `TestOnlyManifestLedger`
  - `TestOnlyCleanupCandidateLedger`
  - `TestOnlySourceCleanupLedger`
- `TestOnlyRebalanceRunner` 用真实 `LocalDiskChunkStore + StorageNodeRegistry + PlacementManager` 执行三阶段：
  - target durable write
  - manifest coordination
  - source cleanup
- 通过 test-only manifest / cleanup ledgers 固定阶段顺序、失败语义和幂等边界，不引入生产 `RebalanceManager` / proto / RPC。

## RebalanceManager contract 覆盖场景

- healthy source + healthy target：target durable 成功后才进入 manifest update
- target durable 失败：不更新 manifest，不清理 source
- manifest update 失败：source 不清理，durable target 进入 cleanup / orphan candidate
- manifest update 成功后才允许 source cleanup
- source cleanup 失败：返回明确 retryable cleanup 状态，manifest 已更新事实不回滚
- unhealthy / stale / overloaded / disk pressure high / capacity 不足 target 被拒绝
- corrupted / quarantined / stale / unavailable source 被拒绝
- target 已有同 checksum chunk 时，durable 阶段视为 already-exists idempotent success，再继续 manifest coordination
- repeated rebalance 幂等，不重复写 manifest，不重复清理 source
- rebalance 不调用 Raft
- rebalance 不把 object payload 写入 metadata / Raft

## target durable before manifest coordination 当前语义

- `TestOnlyRebalanceRunner` 只有在真实 `LocalDiskChunkStore::WriteChunk()` 返回 success / already_exists success 后，才会进入 test-only `TestOnlyManifestLedger::CoordinateMove(...)`
- 如果 target durable 失败：
  - 不调用 manifest ledger
  - 不创建 source cleanup
  - 不伪装成 rebalance 成功

## source cleanup after metadata update 当前语义

- 只有 manifest coordination success 后，才会进入 `TestOnlySourceCleanupLedger::Cleanup(...)`
- manifest update 失败时：
  - source cleanup 不执行
  - durable target 进入 `TestOnlyCleanupCandidateLedger`
- source cleanup 失败时：
  - manifest 已更新事实不回滚
  - 返回明确 retryable cleanup 状态
  - source durable chunk 继续保留，等待后续 cleanup

## test-only rebalance helper 与生产 RebalanceManager 当前边界

- `TestOnlyRebalanceRunner` / `TestOnlyManifestLedger` / `TestOnlyCleanupCandidateLedger` / `TestOnlySourceCleanupLedger` 只存在于 `tests/storage_rebalance_test.cpp`
- 它们不是生产 `RebalanceManager`
- 不新增：
  - `modules/store/maintenance/rebalance_manager.*`
  - Rebalance proto / RPC
  - production manifest coordination
  - read-side repair
  - task persistence
- 当前 helper 只固定 contract，不代表生产 rebalance 编排已完成

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata service
- 不调用 Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot
- rebalance payload 只在 test-only runner 内通过真实 `ReadChunk -> WriteChunk` 在 source/target store 间流转，不进入 metadata / Raft

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_rebalance|rebalance_manager|storage_scrub_repair" --output-on-failure 2>&1 | tee tmp/007/t080-rebalance-manager.log`
  - PASS
  - 实际匹配到的测试名为 `storage_scrub_repair`、`storage_rebalance`
  - 日志路径：`tmp/007/t080-rebalance-manager.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T080 是平台无关 rebalance contract 测试
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮测试涉及真实 chunk copy / durable write / delete；Windows copy / rename / delete / sharing 语义仍待后续实机验证

## 是否通过 T080

- 是

## 是否可以进入 T081

- 可以
- T081 应继续做 `ScrubChunk` / `RepairChunk` proto/RPC 契约，不要把本轮 contract 测试扩展成生产 RebalanceManager

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 rebalance source / target 选择仍依赖 registry snapshot 新鲜度与 test-only manifest ledger 事实，不代表生产 manifest/source-of-truth 新鲜度协议已完成。
- 当前 manifest update 失败后的 cleanup/orphan candidate 只记录在 test-only ledger，尚无生产持久化与重试协议。
- 当前 source cleanup failure 只固定“不回滚 manifest”的 contract，没有生产级 cleanup task persistence / retry scheduler。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只新增测试和文档，没有修改生产模块实现

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T080 完成并记录真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T080 风险条目，保留三阶段一致性、manifest coordination、task persistence、Windows 实机验证等未解风险

## common-risk-notes.md 读取结果

- 已读取
- 未删除生产 RebalanceManager、Rebalance RPC、manifest coordination 真实接入、repair/rebalance task persistence、Windows 实机验证等既有风险
- prerequisites 脚本误指向 006 的问题继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T080`，记录 target durable / manifest update / source cleanup 三阶段仍依赖 test-only ledger、registry snapshot 新鲜度和后续持久化协议
- 删除：
  - 无
- 保留：
  - 生产 RebalanceManager、Rebalance RPC、manifest coordination 真实接入、repair/rebalance task persistence、Windows 实机验证等后续风险继续保留
