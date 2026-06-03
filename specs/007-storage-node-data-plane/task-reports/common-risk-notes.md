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
  问题：`LocalDiskChunkStore` 现已具备生产 `RebuildIndexFromDisk()`、`Initialize()` 自动 live/quarantine index rebuild、stale staging cleanup，以及 read/stat 发现坏块后的 quarantine；但更强的 metadata freshness 判定、后台 scrub/repair 联动和“从未被标记过的 live 文件损坏”主动发现仍未实现。
  影响：重启后现在能重新发现 canonical live final chunk、恢复本地 quarantine 事实并清理 stale/partial staging；但 deleted/deleting 等更完整持久状态恢复、元数据事实新鲜度和主动巡检仍未收口。
  建议后续在哪类任务处理：在后续 scrub/repair、metadata fact source 和 crash matrix 任务中继续收紧 freshness、主动发现和后台治理边界。

- 任务编号：T023
  问题：`LocalDiskChunkStore::WriteChunk()` 现已接入 durable publish，但当前环境没有 Windows 实机验证能力，而 `WindowsDurableFile::SyncDirectory()` 仍是 explicit unsupported，集成后的真实 Windows 成功/失败语义还未验证。
  影响：Linux 上的写入链路已经收口，但在真实 Windows 环境中，WriteChunk 可能表现为 explicit unsupported 或暴露额外的 publish / path / handle 语义偏差。
  建议后续在哪类任务处理：执行 `T023-WIN`，在 Windows 环境完成 `local_disk_chunk_store` 相关 build/test 与必要修正，再关闭该风险。

- 任务编号：T024
  问题：`LocalDiskChunkStore::ReadChunk()` / `StatChunk(verify_checksum=true)` 现在会在发现文件大小或 checksum 与 index metadata 不一致时返回明确错误，并把本地 final chunk 移入 quarantine、更新 index 为 `kQuarantined`；但当前仍没有后台 scrub/failure cache/recent failure scoring，也不会自动修复或向 registry/metadata 传播坏块事实。
  影响：前台读取已经不会把损坏数据当成功返回，重启后也能恢复本地 quarantine 事实；但后续如果需要跨副本短期记忆、后台治理或统一 control-plane 消费，还需要继续补 failure cache、scrub/repair 和事实传播。
  建议后续在哪类任务处理：在后续 read reliability、scrub/repair、failure cache 或 registry-facts 任务中继续收口，不要把当前前台 quarantine 误当成完整坏块治理闭环。

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
  问题：T045 已落地 committed manifest 驱动的最小 read replica selection / fallback，T046 已把测试侧 `ReadObject by manifest` helper 收口到 `tests/support`，T047 固定了 unavailable / not_found / timeout / checksum mismatch fallback，T066 也已把生产 registry facts 接到 read replica selection；但当前仍没有 failure cache、recent failure scoring、corruption 自动状态回写或 repair/scrub 联动。
  影响：仓库现在已经有了 committed-only 的 metadata gate、registry-aware 的副本排序、逐副本 fallback 和可复用测试 helper；但读路径上的坏块沉淀、后台治理和短期失败记忆仍未形成完整生产闭环。
  建议后续在哪类任务处理：在 T048/T052 及后续 recovery-scrub/repair 任务中继续补 failure cache、corruption 沉淀与坏块治理；自动状态回写仍按 T024/后续 recovery-scrub 任务统一处理。

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
  问题：T059 已用 `storage_heartbeat_registry_test` 固定 heartbeat / registry contract；T062-T066 现已落地生产 registry、service、client、write placement 和 read selection 适配，但 clock source 与 sequence 新鲜度仍依赖后续生产路径统一收口。
  影响：如果把当前 registry + service + client + placement/read selection 单测通过当成整个 heartbeat 生产链路已完成，后续仍可能高估 heartbeat facts 的来源一致性和 stale heartbeat 过滤在真实运行时的完备性。
  建议后续在哪类任务处理：在后续 control-plane 可靠性或 time-source 规范任务中继续明确时钟与 sequence 生产语义。

- 任务编号：T060
  问题：T060 已在 `store_placement_policy` / `store_placement_manager` 中固定 health-aware placement contract；T065/T066 已把生产 registry snapshot/facts 分别接到 write-side placement 和 read replica selection，但 failure-domain 更细粒度 spread 以及最终的 overload/hotspot 统一生产打分仍未完成。
  影响：如果把当前 contract 测试、write placement 和 read selection 接线的通过当成所有副本消费面都已完备，后续仍可能高估 registry facts 在 repair/rebalance 等路径中的新鲜度和排序来源。
  建议后续在哪类任务处理：在后续 repair/rebalance/read-side 扩展任务中继续复用 registry facts，并保持 T060 固定下来的稳定选择与排除语义。

- 任务编号：T061
  问题：T061 已在 `proto/storage_node.proto` 中补齐 `RegisterStorageNode`、`UpdateStorageNodeHeartbeat`、`ReportHealth`、`ReportCapacity`、`ReportLoad` 及其 facts/schema；T062-T066 已把 registry、service、client、write placement 和 read selection 接到生产路径，但 fake stub 后续同步和时间语义兼容风险仍存在。
  影响：如果把当前 proto/schema、registry、service、client、placement/read selection 单测通过当成 US4 全量完成，后续仍可能在 fake stub 跟随新接口演进或 time-source 规范上暴露兼容风险。
  建议后续在哪类任务处理：在后续 control-plane 演进任务中继续保持 fake stub 与生成接口同步，并明确时间语义。

- 任务编号：T062
  问题：T062 已实现生产 in-memory `StorageNodeRegistry`，覆盖 register、heartbeat、partial report merge、sequence/stale 保护、liveness 和稳定 snapshot/list；T063-T066 已把这些语义接到 service、client、write placement 和 read selection 侧，但当前时钟来源仍是调用方传入的 `observed_at/now`。
  影响：如果调用方使用漂移较大的时钟、乱序 sequence 或不一致的 report/heartbeat 源，registry + service + client + placement/read selection 仍可能把“新鲜度”判断建立在不可靠输入上；另外 load facts 还没有变成跨所有消费面的统一 overload 生产打分。
  建议后续在哪类任务处理：在后续 control-plane / maintenance 任务中继续统一 freshness 与 overload 的最终消费规则。

- 任务编号：T063
  问题：T063 已实现 `StorageNodeService` 的 `RegisterStorageNode`、`UpdateStorageNodeHeartbeat`、`ReportHealth`、`ReportCapacity`、`ReportLoad` gRPC 适配，并把 proto request/response 映射到 `StorageNodeRegistry`；T064/T066 已补齐 client 和 read-side 消费，但 service 仍直接信任请求携带的 `observed_at_unix_ms`、health/disk/load facts 和 sequence。
  影响：如果后续 client 或调用方在不同时间基准、重复请求、乱序请求或事实缺省策略上不一致，service 虽然会复用 registry 的 stale/idempotent/merge 规则，但整体链路仍可能出现“语义看似一致、输入不够可信”的问题。
  建议后续在哪类任务处理：在 T066 中继续验证 service 暴露出的 snapshot/facts 是否足够支撑 read-side 消费，并决定最终的 time-source / sequence 生产规范。

- 任务编号：T064
  问题：T064 已实现 `StorageNodeClient` 的 `RegisterStorageNode`、`UpdateStorageNodeHeartbeat`、`ReportHealth`、`ReportCapacity`、`ReportLoad` gRPC 调用，并把 gRPC status、proto snapshot/facts 映射回本地 response；但 T061 既有 proto 没有这些 control-plane RPC 的 `timeout_ms` / `best_effort_cancel` 字段，因此当前只有 gRPC `ClientContext` deadline，没有真正的 on-wire cancel hint，也没有自动重试策略。
  影响：如果后续调用方把 T064 误解成已经具备端到端 cancellation propagation、统一重试预算或 service/registry 可观测的 timeout hint，可能高估 control-plane 链路的中断和恢复能力。
  建议后续在哪类任务处理：在后续 control-plane 可靠性或 proto/schema 演进任务中，评估是否需要显式补充 control-plane timeout/cancel 字段、统一 retry 策略和 sequence/time-source 生成协议；T066 继续只消费当前已经固定的 response/facts contract。

- 任务编号：T065
  问题：T065 已把生产 `StorageNodeRegistry` snapshot/facts 接到 write-side `PlacementManager`，T066 也把这套 facts 接到了 read replica selection；但当前 failure-domain 仍只消费 `zone/rack` 占位字段，没有更细粒度 spread 策略，新鲜度依旧取决于调用方提供的 `observed_at/now`。
  影响：如果后续把当前 write/read side 接线误当成所有 placement/read-side/failure-domain 策略都已完备，仍可能高估 zone spread、clock freshness 和 partial facts 在 repair/rebalance 等消费面上的可靠性。
  建议后续在哪类任务处理：在后续 repair/rebalance/read-side 扩展任务中继续复用 registry facts，并决定是否需要更细粒度 failure-domain 策略和更严格的新鲜度协议。

- 任务编号：T066
  问题：T066 已把生产 `StorageNodeRegistry` snapshot/facts 接到 read replica selection，并固定了 healthy/fresh/low-load 优先、stale/unavailable/overloaded/corrupted 跳过、high/full disk pressure 降权、unknown facts 保留 manifest fallback 的 contract；但当前仍没有 failure cache、recent failure scoring、corruption 自动状态回写或 registry snapshot freshness 的独立时钟源。
  影响：如果把当前 registry-aware read selection 当成完整生产读路径治理已完成，后续仍可能在重复失败副本的短期记忆、坏块自动沉淀、以及“snapshot 何时算新鲜”的生产时钟协议上暴露语义缺口。
  建议后续在哪类任务处理：在后续 read reliability、scrub/repair、failure cache 或 time-source 规范任务中继续收口，不要在 T066 里扩展成 repair / corruption 自动回写。

- 任务编号：T068
  问题：T068 已用 test-only restart scanner 固定了“只依据本地磁盘事实重建 live ChunkIndex”的最小 contract；T070-T072 也已经把生产 live rebuild、stale staging cleanup 和 quarantine 恢复落地。但当前 `chunks/live/*.chunk` 仍只持久化 payload bytes，本地磁盘上没有额外 sidecar 或状态编码可直接支撑“原始预期 checksum mismatch”“deleted tombstone 持久化”这类恢复判定。
  影响：如果后续把当前通过误当成“任意 live 文件损坏都能在纯重启扫描阶段自动发现”，仍会高估现有 on-disk facts 的自描述能力；尤其从未被前台读/查触发过的 checksum mismatch，不能仅靠当前 live 文件字节在 rebuild 阶段独立判定。
  建议后续在哪类任务处理：在后续 scrub/repair 或更强 on-disk state encoding 任务中继续补主动发现和状态编码；不要把当前 quarantine 恢复语义误读成全量 corruption 自发现已完成。

- 任务编号：T069
  问题：T069 已新增 cross-platform durability matrix test，明确了 Linux 当前已实测的 `fdatasync` / `fsync` / same-filesystem publish / parent directory sync 语义，以及 Windows 当前只能给出 `FlushFileBuffers`、`MoveFileExW`、replace-existing publish contract、directory durability explicit unsupported 的 contract-only / deferred 边界；但当前 Windows publish 生产实现并没有独立 `ReplaceFileW` 路径，directory durability 也仍是 explicit unsupported。
  影响：如果后续把 T069 的矩阵测试通过误读成“Windows durability 已实机验证完成”，就会高估 `MoveFileExW` replace-existing 与 `ReplaceFileW` 语义等价性，以及目录 durability、sharing violation、long path / UTF-8 path 的真实运行行为。
  建议后续在哪类任务处理：继续在 `T014-WIN`、`T023-WIN` 及后续 cross-platform/crash matrix 任务里做 Windows 实机验证和必要修正；T069 只固定 contract，不关闭 Windows runtime 风险。

- 任务编号：T070
  问题：T070 已实现生产 `RebuildIndexFromDisk()`；T071/T072 也已补上 stale staging cleanup 和 quarantine 恢复。但当前恢复仍主要依赖 canonical `chunks/live/*.chunk` 与 `chunks/quarantine/*.chunk` 的 payload facts，`deleted/deleting` 的持久状态与 Windows `std::filesystem` 目录遍历/路径编码实机行为仍未完成。
  影响：如果后续把当前通过误解成“所有重启恢复语义都已收口”，就会高估 deleted/deleting 等非 live 状态恢复和 Windows 路径语义的完备性；当前保证的是 live final chunk 与已持久化 quarantine facts 可重建。
  建议后续在哪类任务处理：在后续 Windows 恢复验证、scrub/repair 或更强状态编码任务中继续收口 deleted/deleting 和路径行为边界。

- 任务编号：T071
  问题：T071 已实现基于 `last_write_time + staging_cleanup_grace_period_ms` 的 stale staging cleanup，但 mtime 精度、未来时间戳、Windows sharing violation / directory delete 行为，以及“阈值内 fresh staging 是否一定代表可保留”仍是后续运行时边界。
  影响：如果后续把 T071 的通过误解成“所有 staging 恢复和跨平台删除语义都已收口”，就会高估 mtime 判定的稳定性，以及 Windows 上 stale file/empty directory 删除的实机一致性；当前保证的是超过阈值的 staging 能显式清理或显式报错，不是全平台 crash matrix 已完成。
  建议后续在哪类任务处理：在 T072-T074 和后续 Windows 恢复验证任务中继续补 sharing violation、mtime 精度、path encoding 与 crash window 语义；如需要更强的新鲜度判定，再单独演进 staging sidecar 或更明确的 recovery facts。

- 任务编号：T072
  问题：T072 已实现 read/stat 发现坏块后的 quarantine，以及 `RebuildIndexFromDisk()` 对 `chunks/quarantine/` 的恢复；但当前 quarantine 仍依赖前台读/查显式触发，Windows rename/delete/sharing violation 实机行为也未验证。
  影响：如果后续把 T072 的通过误读成“所有损坏 final chunk 都能在纯重启扫描阶段自动发现并稳定隔离”，就会高估当前主动发现能力和跨平台文件移动语义；当前保证的是坏块一旦被前台发现，会显式隔离且重启后保持不可读。
  建议后续在哪类任务处理：在 T073/T074、Windows 实机验证和后续 scrub/repair 任务中继续补 crash window、rename/delete 语义与主动巡检发现能力。
