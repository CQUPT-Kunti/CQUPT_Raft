# T078 ScrubManager Test

## 修改文件

- `tests/storage_scrub_repair_test.cpp`
- `tests/CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/t078-scrub-manager-test.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增 `tests/storage_scrub_repair_test.cpp`，在测试内实现 test-only `TestOnlyScrubRunner`，组合真实 `LocalDiskChunkStore`、真实 `StorageNodeRegistry` snapshot 和真实 `ReplicaPolicySelector`，固定 T078 的最小 scrub contract。
- 用真实 local chunk 文件构造 healthy / corrupted / quarantined / missing replica 场景，验证后台 checksum validation 只负责发现问题、隔离坏副本并产出 repair candidate，不直接 repair。
- 在 `tests/CMakeLists.txt` 注册 `storage_scrub_repair` CTest 入口。

## ScrubManager contract 覆盖场景

- healthy replica checksum 正确时，不输出 repair candidate
- corrupted replica checksum mismatch 时，scrub 触发真实 `LocalDiskChunkStore::StatChunk(verify_checksum=true)` 路径，将坏副本 quarantine，并输出 repair candidate
- quarantined replica 不作为 healthy repair source
- repair candidate 至少包含 `chunk_id`、`expected checksum`、`expected size`、`bad replica`、`healthy source replica`
- 所有 replica 都 corrupted 时，输出明确 `lost_or_unrecoverable` candidate
- under-replicated chunk 输出 repair candidate
- stale / unavailable / unhealthy node 不作为 repair source
- repeated scrub 幂等，重复发现同一 corrupted replica 时结果保持稳定

## corrupted replica / repair candidate 当前语义

- corrupted replica：
  - 对 `LIVE` chunk 执行 `StatChunk(verify_checksum=true)` 时，如本地最终文件 size/checksum 与 index metadata 不一致，真实 store 会把副本移入 `chunks/quarantine/`，index 状态更新为 `kQuarantined`
  - test-only scrub 读取到该事实后，把该 replica 记为 `known_corrupted`
- repair candidate：
  - 只表达问题和可用修复来源，不执行 copy / repair
  - 当前字段包括：
    - `chunk_id`
    - `expected_size`
    - `expected_checksum`
    - `bad_replicas`
    - `healthy_source_replicas`
    - `under_replicated`
    - `lost_or_unrecoverable`

## test-only scrub helper 与生产 ScrubManager 当前边界

- `TestOnlyScrubRunner` 只存在于 `tests/storage_scrub_repair_test.cpp`
- 它不是生产 `ScrubManager`，不新增 `modules/store/maintenance/scrub_manager.*`
- 它只消费：
  - test-only manifest facts
  - 真实 `LocalDiskChunkStore`
  - 真实 `StorageNodeRegistry`
  - 真实 `ReplicaPolicySelector`
- 它不实现：
  - 后台任务队列
  - retry / persistence
  - ScrubChunk RPC
  - RepairManager
  - RebalanceManager
  - read-side repair

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不调用 `RaftNode::ProposeMetadata()`
- 不把 object payload 写入 metadata / Raft / snapshot
- repair candidate 只保存 expected checksum / size 和 replica facts，不保存 object payload

## 是否使用 tests/test_file/test_file.zip

- 否
- 本任务只使用 `MakeChunkPayload(...)`

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_scrub_repair|scrub_manager|scrub_repair" --output-on-failure 2>&1 | tee tmp/007/t078-scrub-manager.log`
  - PASS
  - 实际匹配到的测试名为 `storage_scrub_repair`
  - 日志路径：`tmp/007/t078-scrub-manager.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T078 是平台无关 scrub contract 测试
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本轮测试使用了真实文件损坏与 quarantine 路径，Windows rename / quarantine / 文件占用语义仍待后续实机验证

## 是否通过 T078

- 是

## 是否可以进入 T079

- 可以
- T079 应继续做 RepairManager 测试，不要把本轮 test-only scrub contract 扩展成生产 Repair / Scrub 实现

## 当前任务发现的不合理点 / 警告 / 风险

- 当前 scrub candidate 仍依赖 test-only manifest snapshot、registry snapshot 和本地 quarantine 状态的组合观察，尚未定义生产级 snapshot boundary / freshness 协议。
- `ReplicaPolicySelector::SelectReadReplicas()` 是 read-side 排序语义；T078 测试在其基础上进一步收紧“repair source 必须是 live + healthy”的过滤，但这还不是生产 RepairManager 的最终 source policy。
- 当前没有 registry failure cache、recent failure scoring、ScrubChunk RPC、后台低优先级 scrub queue、task persistence 或 repair 执行。

## 是否更新 module-notes.md / AGENTS.md

- 否
- 本任务只补测试和文档，没有修改生产模块实现

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：标记 T078 完成并记录真实修改范围/验收
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：新增 T078 风险条目，保留 scrub candidate 与 manifest / registry / quarantine 状态之间的新鲜度与一致性风险

## common-risk-notes.md 读取结果

- 已读取
- 未删除生产 ScrubManager、ScrubChunk RPC、RepairManager、RebalanceManager、read-side repair、registry failure cache、Windows 实机验证等既有风险
- prerequisites 脚本误指向 006 的问题继续保留

## common-risk-notes.md 新增/删除/保留情况

- 新增：
  - `T078`，记录 test-only scrub candidate 已有 contract 测试，但 manifest / registry / quarantine 事实之间的 freshness / consistency 仍未定义
- 删除：
  - 无
- 保留：
  - 生产 ScrubManager、ScrubChunk RPC、RepairManager、RebalanceManager、read-side repair、registry failure cache、Windows 实机验证等后续风险继续保留
