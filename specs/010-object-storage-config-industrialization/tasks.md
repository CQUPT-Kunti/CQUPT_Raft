# Tasks: Object Storage Config Industrialization

**Input**: Design documents from `/specs/010-object-storage-config-industrialization/`  
**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/](contracts/)

**Tests**: Tests are required because the feature changes placement, upload concurrency, manifest correctness and read fallback behavior.

**Organization**: Tasks are grouped by user story and capped to the first-stage scope.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel after dependencies are satisfied.
- **[Story]**: Maps to spec user stories.
- Every task includes code change, test change and verification.

## Phase 1: Foundation

- [X] T001 [US4] Unify production chunk-size default and remove config-driven chunk_size semantics in `modules/store/common/store_types.h`, `apps/storage_client.cpp`, and targeted storage/client tests.

修改目标：新增或复用一个 store/common 代码级 production chunk-size 入口，消除 `apps/storage_client.cpp` 的 4MiB 重复默认和 `chunk_size_bytes` 配置语义读取。  
涉及文件：`modules/store/common/store_types.h`、`modules/store/common/store_types.cpp` 如需、`apps/storage_client.cpp`、相关 `tests/*config*` 或 storage client parsing tests。  
预期行为：上传默认 chunk size 来自单一代码常量，配置文件中的 `chunk_size_bytes` 不再改变上传语义。  
验收标准：仓库内生产路径不再有多个 chunk size 默认值；测试验证配置有无 `chunk_size_bytes` 都使用同一默认。  
测试方式：targeted unit test + `cmake --preset debug-ninja-low-parallel`。

## Phase 2: User Story 1 - Per-Chunk Dynamic Placement (Priority: P1)

- [X] T002 [P] [US1] Remove node-id/name/config-order placement priority and add deterministic chunk-scoped distribution in `modules/store/placement/replica_policy.cpp` with placement policy tests.

修改目标：资源过滤和排序保持 resource-aware；等价资源条件下使用 chunk-scoped deterministic jitter，禁止 lexical `node_id` tie-break 和 config-order fallback。  
涉及文件：`modules/store/placement/replica_policy.h` 如需、`modules/store/placement/replica_policy.cpp`、`modules/store/placement/module-notes.md`、`tests/store_placement_policy_test.cpp`。  
预期行为：9 个健康节点、多 chunk placement 覆盖大多数节点，不同 chunk 可以得到不同 replica set。  
验收标准：打乱 node_id/name/config order 不会出现固定取前 N 节点；测试可用 deterministic seed 复现。  
测试方式：`store_placement_policy_test`。

- [X] T003 [US1] Make CreateWritePlan express per-chunk selected replica nodes by extending existing transfer plan facts in `modules/store/transfer/object_transfer.h`, `modules/store/transfer/metadata_transfer_client.*`, and plan/manifest tests.

修改目标：复用 `TransferWritePlan` / `TransferChunkPlan` 表达 chunk_size、total_chunks、replica_count、minimum_successful_writes、placement_epoch、expiry 和 selected replica nodes。  
涉及文件：`modules/store/transfer/object_transfer.h`、`modules/store/transfer/metadata_transfer_client.h`、`modules/store/transfer/metadata_transfer_client.cpp`、必要时 `proto/metadata.proto` / `proto/common.proto` 仅做 additive payload-free 扩展、`tests/metadata_manifest_test.cpp`、transfer adapter tests。  
预期行为：CreateWritePlan 返回 chunk-level dynamic placement；缺少 selected replica nodes 时 upload 不能继续写 payload。  
验收标准：每个 chunk plan 有 selected replica nodes；新增核心 struct 数量为 0，除非实现说明 1 个内部 helper 的必要性。  
测试方式：metadata transfer/write-plan test + descriptor/manifest regression if proto touched。

- [X] T004 [US1] Make upload consume selected plan nodes and remove silent sorted-target fallback in `modules/store/transfer/object_transfer.cpp` with upload integration tests.

修改目标：upload second pass 只写 CreateWritePlan 选出的 nodes；selected node 缺失或不可发现时返回明确 plan/discovery error。  
涉及文件：`modules/store/transfer/object_transfer.cpp`、`modules/store/transfer/module-notes.md`、`tests/storage_upload_integration_test.cpp`、`tests/storage_upload_coordinator_test.cpp` 如需同步 helper。  
预期行为：upload 不再使用 `SortedStorageTargets` 以 node_id 补齐缺失副本；CommitObject manifest 保存实际 durable replica nodes。  
验收标准：测试证明 plan 中不同 chunk 的 selected replica sets 被执行，manifest 的 `replica_nodes` 等于 durable success nodes。  
测试方式：`storage_upload_integration_test` 和 `storage_upload_coordinator_test` targeted cases。

## Phase 3: User Story 2 - Bounded High-Concurrency Upload (Priority: P1)

- [X] T005 [US2] Add parallel per-chunk replica fan-out with minimum_successful_writes aggregation in `modules/store/transfer/object_transfer.cpp` and fan-out tests.

修改目标：同一 chunk 的 replica writes 并行执行，成功条件由 `minimum_successful_writes` 控制，慢/失败副本进入诊断和 cleanup/repair candidate facts。  
涉及文件：`modules/store/transfer/object_transfer.cpp`、`modules/store/transfer/object_transfer.h` 如需诊断字段、`tests/storage_upload_integration_test.cpp`、`tests/support/storage_upload_test_utils.h`。  
预期行为：两个 durable success 可让 chunk commit-eligible，non-retryable 和 retryable failure 被聚合且不误提交失败副本。  
验收标准：阻塞 writer 测试观察到 same-chunk 多副本 overlap；minimum_successful_writes 未满足时不调用 CommitObject。  
测试方式：fan-out unit/integration test + existing failed upload cleanup tests。

- [X] T006 [US2] Add bounded multi-chunk upload pipeline with max_inflight_chunks and max_inflight_bytes backpressure in `modules/store/transfer/object_transfer.cpp` and concurrency tests.

修改目标：第二遍上传在读取 chunk payload 前获取 chunk/byte budget；完成后释放预算；不得把整文件读入内存。  
涉及文件：`modules/store/transfer/object_transfer.cpp`、`modules/store/transfer/object_transfer.h` 如需 session diagnostics、`modules/store/runtime/storage_executor.*` 仅在复用有界执行器时修改、`tests/integrated_object_storage_concurrency_test.cpp`。  
预期行为：多个 chunk 可同时上传，但 in-flight chunk 数和 payload 字节数始终受限，预算耗尽时形成 backpressure。  
验收标准：测试记录峰值 in-flight chunks/bytes 不超过配置；大于预算的 reader 不继续预读。  
测试方式：`integrated_object_storage_concurrency_test` targeted cases。

## Phase 4: User Story 3 - Manifest Read And Repair-Ready Facts (Priority: P2)

- [X] T007 [P] [US3] Add production manifest replica fallback reads in `modules/store/transfer/object_transfer.cpp` using existing read-replica policy and read integration tests.

修改目标：download 对每个 chunk 按 manifest replica list 和可用健康事实排序，首个副本失败时尝试同 chunk 其他副本并校验 checksum。  
涉及文件：`modules/store/transfer/object_transfer.cpp`、`modules/store/placement/replica_policy.cpp` 如需 read ordering 补强、`tests/storage_read_integration_test.cpp`、`tests/support/storage_read_test_utils.h`。  
预期行为：读取只使用 committed manifest replica nodes，不从非 manifest discovery 节点猜测数据。  
验收标准：首选副本 not found/checksum mismatch 时，下载能从第二健康副本成功；所有副本失败时返回聚合错误。  
测试方式：`storage_read_integration_test` targeted fallback cases。

- [X] T008 [US3] Preserve repair-B-ready manifest and placement facts in `modules/store/placement/*`, `modules/store/maintenance/repair_manager.*`, and manifest/repair direction tests without implementing full repair loop.

修改目标：placement 支持排除已有健康副本和不可写节点后重新选择新目标；repair task/fact 不假设必须补回原节点。  
涉及文件：`modules/store/placement/placement_manager.*`、`modules/store/placement/replica_policy.*`、`modules/store/maintenance/repair_manager.*` 如需只调整 facts、`tests/store_placement_manager_test.cpp`、`tests/storage_scrub_repair_test.cpp`。  
预期行为：缺失副本场景可重新选择当前更合适节点，manifest 仍记录实际 durable replicas。  
验收标准：测试验证 original missing node 不可写/容量不足时，新 target 可为其他健康节点；不触发 metadata manifest coordination 的完整 repair 实现。  
测试方式：placement manager repair-B scenario + existing repair manager boundary tests。

## Dependencies & Execution Order

- T001 blocks T006 defaults and CLI behavior.
- T002 blocks T003/T004 because plan generation must rely on corrected placement.
- T003 blocks T004 because upload execution needs selected plan nodes.
- T004 blocks T005/T006 because concurrency must execute the correct plan, not fallback targets.
- T007 can start after T003 because read manifest facts must be clear.
- T008 can start after T002 and T004 because repair direction depends on placement and manifest facts.

## Implementation Strategy

MVP is T001 to T006. Stop after T006 and validate upload correctness, manifest facts, fan-out and bounded in-flight behavior. Then add T007 read fallback and T008 repair-ready direction.

## Validation Commands

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
ctest --test-dir build/debug-ninja-low-parallel --output-on-failure -R "store_placement|storage_upload|storage_read|metadata_manifest|integrated_object_storage_concurrency"
```

If full validation is needed:

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```
