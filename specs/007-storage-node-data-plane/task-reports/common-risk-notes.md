# Common Risk Notes

- 任务编号：T001
  问题：`.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 当前返回的 `FEATURE_DIR` 是 `specs/006-remove-kv-metadata-state-machine`，与本次执行的 `specs/007-storage-node-data-plane` 不一致。
  影响：继续依赖该脚本可能把实现流程、自动化检查或扩展 hook 引导到错误 feature 目录。
  建议后续在哪类任务处理：在后续 speckit 工作流或脚本修正类任务中校正 feature 选择逻辑，避免 007 任务执行时误绑定到 006。

- 任务编号：T009
  问题：`tests/support/store_test_utils.h` 中的 `MakeChecksumFixture()` 仍返回 `fixture-fnv1a:*` 形式，与当前 `storedemo::ComputeChunkChecksum()` 的 SHA-256 生产语义不一致。
  影响：后续 storage 集成测试如果继续复用该 helper，可能误把测试夹具摘要当成生产 checksum，导致用例语义漂移或断言失真。
  建议后续在哪类任务处理：在后续 storage 测试工具或 LocalDiskChunkStore 测试任务中统一切到生产 checksum helper，或显式区分 fixture checksum 与 production checksum。

- 任务编号：T014
  问题：`WindowsDurableFile` 已完成条件编译实现和 Windows 条件测试，但当前环境没有 Windows 编译/测试能力，`MoveFileExW`、long path、UTF-8 path 和 directory durability 的实机行为仍未验证。
  影响：如果直接把当前状态当成跨平台实机通过，后续可能在真实 Windows 机器上暴露路径转换、句柄共享或 durability 语义偏差。
  建议后续在哪类任务处理：执行 `T014-WIN`，在真实 Windows 环境完成 build/test 和必要修正，再关闭该风险。

- 任务编号：T016
  问题：`ShardedChunkIndex::List()` 当前使用 `chunk_id` 字典序和 `page_token` 继续翻页，但还没有为并发修改提供稳定快照分页保证。
  影响：T018 已补 per-chunk 串行化和基础容器并发保护，但跨页扫描在并发修改下仍可能出现页边界漂移或重复/漏读，需要明确 snapshot 语义或更强一致性策略。
  建议后续在哪类任务处理：在后续索引并发/恢复任务中继续收紧分页一致性语义，必要时引入 snapshot pinning 或更强 page token 结构。

- 任务编号：T018
  问题：T026 已用真实并发压力覆盖 `LocalDiskChunkStore::WriteChunk()` / `DeleteChunk()` 的 chunk guard 主路径，但 `AcquireChunkLock()` 的跨步骤原子性仍依赖未来 repair/rebalance/recovery 等新增业务入口继续显式持有 guard。
  影响：当前 store 主写删入口的并发串行边界已经被测试固定；如果后续新入口遗漏 guard，只依赖索引内部容器锁，仍可能在多步骤状态转换上出现交错。
  建议后续在哪类任务处理：在后续 repair/rebalance/recovery/store 业务扩展任务中，继续把 chunk guard 作为多步骤状态切换的固定前置步骤，并补对应测试。

- 任务编号：T019
  问题：`BoundedStorageExecutor` 已通过 T020 测试固定当前“任务上下文不自动触发运行中取消”的边界，但仍没有实现 deadline 到点取消或任务结果 future 聚合。
  影响：后续 `LocalDiskChunkStore` 和 StorageNodeService 如果把 timeout/cancellation 当成已生效的运行中中断能力，仍可能高估当前 runtime 的回收与中止能力。
  建议后续在哪类任务处理：在后续 runtime 接入任务中，继续把 timeout/cancellation 当作扩展点；如需要真正的取消传播，再单独收紧接口和测试。

- 任务编号：T019
  问题：`BoundedStorageExecutor::Shutdown()` 已通过 T020 测试固定为 owner-thread 模型：worker 回调内部调用会返回明确边界错误，不支持自停或自析构。
  影响：如果后续业务代码忽略这条约束，仍可能在停机收口或对象生命周期管理上误用执行器。
  建议后续在哪类任务处理：在后续 store runtime / LocalDiskChunkStore 接入任务中，继续按 owner 线程停机模型使用；如确实需要 worker 内触发停机，再单独演进生命周期协议。

- 任务编号：T021
  问题：`LocalDiskChunkStore` 当前只完成配置持有和目录初始化，还没有做 restart scan、stale staging cleanup、index rebuild 或 live chunk 发现。
  影响：如果后续任务在没有补齐这些恢复路径前就把它当成“可恢复本地存储”使用，重启后可能看不到已有 live chunk，也不会自动处理遗留 staging。
  建议后续在哪类任务处理：在 T022/T023 收紧基础测试和写入入口边界，在 T070/T071 及相关恢复任务中补齐 restart rebuild 与 staging cleanup。

- 任务编号：T023
  问题：`LocalDiskChunkStore::WriteChunk()` 现已接入 durable publish，但当前环境没有 Windows 实机验证能力，而 `WindowsDurableFile::SyncDirectory()` 仍是 explicit unsupported，集成后的真实 Windows 成功/失败语义还未验证。
  影响：Linux 上的写入链路已经收口，但在真实 Windows 环境中，WriteChunk 可能表现为 explicit unsupported 或暴露额外的 publish / path / handle 语义偏差。
  建议后续在哪类任务处理：执行 `T023-WIN`，在 Windows 环境完成 `local_disk_chunk_store` 相关 build/test 与必要修正，再关闭该风险。

- 任务编号：T024
  问题：`LocalDiskChunkStore::ReadChunk()` 已经固定为 full read + checksum on read，但当前发现文件大小或 checksum 与 index metadata 不一致时，只返回明确错误，还不会把本地 index 状态自动写回 `CORRUPTED` / `QUARANTINED`。
  影响：前台读取已经不会把损坏数据当成功返回，但后续如果需要基于读路径直接沉淀损坏事实、触发隔离或给 scrub/repair 复用，还需要补状态回写与恢复协同。
  建议后续在哪类任务处理：在后续 T025/T恢复/scrub 相关任务中，再统一收紧 corruption 状态写回、隔离和恢复边界。

- 任务编号：T025
  问题：`LocalDiskChunkStore::DeleteChunk()` 现已接入真实文件删除和 `DELETED` index 状态更新，但当前环境没有 Windows 实机验证能力，`std::filesystem::remove` 在 sharing violation / open-handle / unlink 语义上的真实行为仍未验证。
  影响：Linux 上删除、Stat、List 语义已经收口，但在真实 Windows 环境中，DeleteChunk 可能暴露额外的 sharing violation、删除失败分类或文件可见性差异。
  建议后续在哪类任务处理：执行 `T025-WIN`，在 Windows 环境完成 `local_disk_chunk_store` 相关 build/test 与必要修正，再关闭该风险。

- 任务编号：T026
  问题：T026 已在 Linux 上覆盖真实 chunk 文件的高并发 write/read/delete/stat/list 压力，但当前环境没有 Windows 编译/测试能力，Windows 下的 sharing violation、open-handle delete、并发读删可见性和 durable publish 后读取差异仍未验证。
  影响：Linux 上不同 chunk 并行、同 chunk 冲突控制和 bounded backpressure 已有实测证据；如果直接把这组结果外推到 Windows，后续仍可能在真实 NTFS/Win32 文件语义下暴露并发偏差。
  建议后续在哪类任务处理：执行 `T026-WIN`，在 Windows 环境完成 `store_concurrency_stress` 实机验证，并结合 `T023-WIN` / `T025-WIN` 收口必要修正。

- 任务编号：T027
  问题：T027 只新增了 `MetadataStateMachine + LocalDiskChunkStore` 的上传闭环集成测试骨架，当前仓库仍没有真实 upload coordinator / StorageNode RPC 来强制“chunk durable 成功后才能 CommitObject”。如果调用方绕过预期顺序，或在 durable chunk 写成后 metadata commit 失败，当前会留下未被 metadata 引用的 orphan chunk。
  影响：测试已经证明 commit 前对象不可见、commit 后可见，但 metadata commit 失败后的本地 chunk 仍会保留在 store data-plane，直到后续 abort/GC/recovery 任务补齐前都可能累积无主 chunk。
  建议后续在哪类任务处理：在 T029-T035 的真实 upload coordinator / service / placement / abort-or-cleanup 任务中，把 commit gate 和 failed-commit orphan cleanup 收紧为生产语义，并补对应集成回归。

- 任务编号：T028
  问题：T028 的 `WriteChunk` contract 已在 T031 接入真实 `StorageNodeService::WriteChunk`，但当前仓库仍没有 `StorageNodeClient`，也没有跨 RPC 的 deadline/cancelled 映射收口；`timeout_ms` / `best_effort_cancel` 仍只是显式边界字段。
  影响：如果后续 T032 在 client 接入时把 `already_exists`、`conflict`、`overloaded`、deadline 或 cancellation 解释成与 T028/T031 contract 不一致的 RPC 行为，仍可能在重试、过载回退或模糊超时下出现语义漂移。
  建议后续在哪类任务处理：在 T032 的 StorageNode client 实现与回归测试中，对齐 T028/T031 contract，并补 RPC deadline/cancelled/error mapping 的真实端到端验证。
