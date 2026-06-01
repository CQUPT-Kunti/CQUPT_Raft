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
  问题：`LocalDiskChunkStore::ReadChunk()` 已经固定为 full read + checksum on read；T047 也补了“checksum mismatch 不向上层返回有效 payload、quarantined/corrupted 状态拒读、坏文件读取后不自动把 index 改写为 quarantine”的测试边界。但当前发现文件大小或 checksum 与 index metadata 不一致时，store 仍只返回明确错误，还不会把本地 index 状态自动写回 `CORRUPTED` / `QUARANTINED`。
  影响：前台读取已经不会把损坏数据当成功返回，测试层也已经把 quarantine/corrupted 的拒读和 fallback 边界固定；但后续如果需要基于读路径直接沉淀损坏事实、触发真实隔离、后台清理或给 scrub/repair 复用，还需要补状态回写与恢复协同。
  建议后续在哪类任务处理：在后续 T恢复/scrub/repair 相关任务中，再统一收紧 corruption 状态写回、隔离和恢复边界。

- 任务编号：T025
  问题：`LocalDiskChunkStore::DeleteChunk()` 现已接入真实文件删除和 `DELETED` index 状态更新，但当前环境没有 Windows 实机验证能力，`std::filesystem::remove` 在 sharing violation / open-handle / unlink 语义上的真实行为仍未验证。
  影响：Linux 上删除、Stat、List 语义已经收口，但在真实 Windows 环境中，DeleteChunk 可能暴露额外的 sharing violation、删除失败分类或文件可见性差异。
  建议后续在哪类任务处理：执行 `T025-WIN`，在 Windows 环境完成 `local_disk_chunk_store` 相关 build/test 与必要修正，再关闭该风险。

- 任务编号：T026
  问题：T026 已在 Linux 上覆盖真实 chunk 文件的高并发 write/read/delete/stat/list 压力，但当前环境没有 Windows 编译/测试能力，Windows 下的 sharing violation、open-handle delete、并发读删可见性和 durable publish 后读取差异仍未验证。
  影响：Linux 上不同 chunk 并行、同 chunk 冲突控制和 bounded backpressure 已有实测证据；如果直接把这组结果外推到 Windows，后续仍可能在真实 NTFS/Win32 文件语义下暴露并发偏差。
  建议后续在哪类任务处理：执行 `T026-WIN`，在 Windows 环境完成 `store_concurrency_stress` 实机验证，并结合 `T023-WIN` / `T025-WIN` 收口必要修正。

- 任务编号：T027
  问题：T035 已补最小 `UploadCoordinator`，能够按 `CreateObject -> Placement -> WriteChunk -> CommitObject` 顺序串联 metadata/control-plane 与 StorageNode/data-plane，但当前仍没有 `AbortObject`、GC、重试调度或重启恢复协同。只要 `CreateObject` 已成功而后续 placement/write/commit 失败，对象就可能长期停留在 metadata `PENDING`；只要有 durable chunk 而 `CommitObject` 未成功，就仍可能留下 orphan chunk。
  影响：仓库现在已经有了明确的 commit gate，不再完全缺少 upload coordinator；但失败路径仍依赖调用方或后续 GC/recovery 任务处理 pending object 和 orphan chunk，尚不能把这部分状态自动收口成生产语义。
  建议后续在哪类任务处理：T036 已把 commit manifest 必须等于 durable success facts 的契约测试固定，T037 已把“未达到最小成功副本数时生成 cleanup candidate”的边界固定；后续仍需在 abort/GC/recovery 任务中补齐真实 cleanup 执行、pending object 收口和重启恢复协同。

- 任务编号：T045
  问题：T045 已落地 committed manifest 驱动的最小 read replica selection / fallback，T046 已把测试侧 `ReadObject by manifest` helper 收口到 `tests/support`，T047 也补了 unavailable / not_found / timeout / checksum mismatch fallback、全部副本失败和已知坏副本跳过的失败路径覆盖；但当前 selector 仍主要消费 manifest 顺序和调用方传入的最小候选事实，尚未接入真实 heartbeat / registry / failure cache；读路径也仍保持 `LocalDiskChunkStore` 的既有边界：range read 由底层显式拒绝，checksum mismatch / corrupted 只返回明确错误，不自动回写 `CORRUPTED` / `QUARANTINED` 状态。
  影响：仓库现在已经有了 committed-only 的 metadata gate、副本顺序选择、逐副本 fallback、可复用测试 helper 和失败路径测试边界，后续任务不能再随意改动 T041-T047 固定下来的字段、状态和失败扩散语义；但当前还没有基于实时节点事实的更强 read selection，也没有读路径上的 corruption 自动沉淀、repair、scrub 或后台 quarantine 动作。
  建议后续在哪类任务处理：在 T048/T052/T066 及后续 recovery-scrub 任务中继续接入 registry facts、failure scoring 与坏块治理；corrupted 状态自动回写仍按 T024/后续 recovery-scrub 任务统一处理。

- 任务编号：T049
  问题：T049 已用 `storage_delete_gc_test` 固定 `DeleteObject -> invisible -> test-only cleanup candidate / GC safety helper` 的测试边界，也覆盖了 committed live manifest 保护、重复删除重放和 failed upload orphan candidate；但当前仍不是完整后台 GC 生命周期闭环。
  影响：如果把 T049 的测试 helper 和定向验证当成 US3 全量生产删除/GC 已完成，后续会高估 metadata tombstone 之后的真实后台回收能力；当前通过的是 metadata-first、candidate generation、metadata-driven safety 和最小 restart resume contract，不是 repair/rebalance/scrub 或全平台删除语义落地。
  建议后续在哪类任务处理：在后续 Windows 删除语义、timeout/cancellation 运行中传播和更完整后台维护任务中继续收口。

- 任务编号：T055
  问题：T055 已在生产 `GarbageCollector` 中加入必需的 metadata-driven safety checker gate，并固定“live manifest 引用时不调用 delete handler、checker 暂时不可用时按 retryable 失败处理”的边界；但当前 safety checker 仍依赖调用方注入外部 metadata 事实源，Windows 实机删除验证也未完成。`next_retry_after_ms` 仍只是任务模型扩展点，尚未形成真正的延迟重试调度器；`best_effort_cancel` / timeout 运行中传播同样未打通到 service/store 删除执行。
  影响：如果把当前 safety gate 通过当成完整生产 GC 已完成，后续会高估延迟重试、跨平台删除验证和 metadata fact freshness 保证能力；另外 live-manifest 保护的正确性仍取决于调用方提供的 metadata 事实源是否新鲜且完整。
  建议后续在哪类任务处理：在后续真实 metadata fact source 接线、Windows 验证和 timeout/cancellation 运行中传播任务中继续收口。

- 任务编号：T056
  问题：T056 已补 pending timeout、failed upload、abort cleanup、deleted object cleanup 的 generic candidate generation，并固定 candidate -> `GarbageCollectorTask` 转换、排序和去重边界；但当前 candidate 正确性仍依赖调用方提供足够新鲜的 metadata snapshot、object state 和 timeout 事实。
  影响：如果调用方在过期 metadata 视图或不稳定时间基准上生成 candidate，仍可能产生重复候选、延迟候选或在 safety gate 阶段再被拒绝。
  建议后续在哪类任务处理：在后续真实 metadata fact source 接线任务中继续收紧 candidate 生成的快照新鲜度边界。

- 任务编号：T057
  问题：T057 已补最小 GC task snapshot persistence 和 restart resume，并通过 `DurableFile` staging/publish/sync 收口 Linux 当前路径；T057-FIX 已把保存阶段从“先拼完整 payload”改成 streaming append，降低了保存时的内存峰值，但当前 persistence 仍是 whole-snapshot rewrite，没有 schema migration 机制，也没有多进程并发访问协议。Windows 下 `SyncDirectory()` 仍是 explicit unsupported，真实持久化语义需要单独验证。
  影响：保存时不再额外构造整份 snapshot 字符串，但后续如果在 schema 演进、跨版本兼容、多进程共享同一 persistence root，或真实 Windows durability 语义上继续扩展，当前实现仍可能暴露 snapshot 覆盖、磁盘写放大、兼容性或 directory durability 边界。
  建议后续在哪类任务处理：在后续 T057-WIN 或跨平台持久化验证任务中完成 Windows 实机验证，并在需要跨版本演进时补 schema migration / compatibility 策略。

- 任务编号：T059
  问题：T059 已用 `storage_heartbeat_registry_test` 固定 test-only heartbeat / registry contract，包括注册幂等、endpoint conflict、heartbeat sequence 去重、stale heartbeat 忽略和基于 `last_seen + timeout` 的 liveness；但当前仍没有生产 `StorageNodeRegistry`、proto、service、client 或 Placement/read-side 的真实接线，clock source 与 sequence 新鲜度也仍依赖后续生产实现统一收口。
  影响：如果把当前 test-only adapter 的通过当成真实 registry 已完成，后续可能高估 heartbeat facts 的来源一致性、stale heartbeat 过滤和 liveness 降级在生产路径里的完备性。
  建议后续在哪类任务处理：在 T061-T066 中继续把 proto/schema、生产 registry、service/client、Placement eligibility 和 read replica selection 接线收口，并明确 clock / sequence 的最终生产语义。

- 任务编号：T060
  问题：T060 已在 `store_placement_policy` / `store_placement_manager` 中用 test-only registry facts 固定 health-aware placement contract，包括 stale heartbeat 事实降级、unhealthy/readonly/draining/overloaded/high-disk-pressure/insufficient-capacity 排除、healthy 低负载节点优先和 duplicate node_id 去重；但当前仍没有生产 registry -> PlacementManager 的真实接线，也还没有最终的 overload/disk-pressure/hotspot 生产打分来源。
  影响：如果把当前 contract 测试的通过当成真实 placement eligibility 已完成，后续可能高估 registry facts 在生产路径中的新鲜度、一致性和最终排序来源。
  建议后续在哪类任务处理：在 T062/T065 中继续把生产 registry、liveness clock、capacity/load/disk-pressure facts 和 manager 接线收口，并保持 T060 固定下来的稳定选择与排除语义。
