# Tasks: Strong Consistency Metadata Layer

**Input**: `specs/005-strong-consistency-metadata-layer/spec.md`、`plan.md`、`data-model.md`、`api.md`、`client-design.md`、`validation-matrix.md`、`research.md`  
**Branch**: `005-strong-consistency-metadata-layer`  
**Mode**: 本文件只生成任务清单，不实现代码。执行任一任务前必须先读取根 `NOTREAD.md`，并以当时的禁止路径为最高优先级约束。

## Guardrails

- 不读取 `NOTREAD.md` 禁止路径；如果任务中列出的“允许读取文件”与当时 `NOTREAD.md` 冲突，必须停止并请求边界确认。
- 不全量扫描仓库；每个任务只能读取自己列出的文件。
- 不实现 StorageNode、真实 chunk 文件存储、大文件真实上传下载、chunk replication、纠删码、rebalance 或 S3 协议。
- 不把真实大文件数据写入 Raft log；`payload` 只允许 metadata-only 小字段。
- 不重构 Raft 内核；`modules/raft/node`、`modules/raft/replication`、`modules/raft/storage` 默认不作为修改对象。
- 一个任务不得同时修改状态机、客户端、测试和文档。
- Linux 与 Windows 验证任务必须分别执行并分别记录结果。

## Phase 1: Setup (Shared Boundary Confirmation)

**Purpose**: 固定本 feature 的实现边界，避免从 KV demo 演进时误触 Raft 内核或禁止路径。

- [x] T001 Establish implementation boundary notes in specs/005-strong-consistency-metadata-layer/tasks.md
  - 目标: 在开x始实现前确认本 feature 只触碰上层 metadata demo/API/client/state_machine 规划边界。
  - 允许读取文件: `NOTREAD.md`, `AGENTS.md`, `specs/005-strong-consistency-metadata-layer/plan.md`, `specs/005-strong-consistency-metadata-layer/spec.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/tasks.md`
  - 实现要求: 记录不得读取 004、不得读取禁止路径、不得修改 Raft 内核、不得实现数据面。
  - 验收标准: `tasks.md` 中存在明确 guardrails；未修改源码、测试或协议文件。

- [X] T002 [P] Map current KV demo boundaries in specs/005-strong-consistency-metadata-layer/tasks.md
  - 目标: 明确 KV demo/client/state_machine 与 Raft 内核的边界，作为后续实现任务输入。
  - 允许读取文件: `NOTREAD.md`, `AGENTS.md`, `apps/AGENTS.md`, `modules/raft/common/AGENTS.md`, `modules/raft/state_machine/AGENTS.md`, `modules/raft/service/AGENTS.md`, `proto/AGENTS.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/tasks.md`
  - 实现要求: 只总结允许文件中的模块职责，不读取源码、不读取 tests、不读取 004。
  - 验收标准: 明确 `common` 负责 command codec、`state_machine` 负责业务状态、`service` 负责适配、`apps` 负责 CLI、`proto` 负责契约、Raft 内核不主动修改。

- [X] T003 [P] Confirm future touched file list in specs/005-strong-consistency-metadata-layer/tasks.md
  - 目标: 为后续任务固定最小文件范围，避免全量扫描。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/plan.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/tasks.md`
  - 实现要求: 列出未来可触碰的候选路径: `modules/raft/common/*metadata*`, `modules/raft/state_machine/*metadata*`, `modules/raft/service/*metadata*`, `proto/raft.proto`, `apps/*metadata*_client.cpp`, 指定测试文件和 CMake wiring。
  - 验收标准: 每个后续任务都有具体允许读取和允许修改路径，不出现“扫描整个模块”。

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: 建立数据模型、命令契约和公共错误语义；未完成前不得进入用户故事实现。

- [x] T004 Define metadata domain header in modules/raft/common/metadata_command.h
  - 目标: 建立 `MetadataRecord`、`MetadataRecordState`、`MetadataCommand`、`IdempotencyEntry`、`Tombstone` 的接口与轻量类型。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/common/AGENTS.md`, `modules/raft/common/command.h`, `specs/005-strong-consistency-metadata-layer/data-model.md`
  - 允许修改文件: `modules/raft/common/metadata_command.h`
  - 实现要求: `.h` 只放类型、枚举、常量、函数声明和轻量 inline；不放复杂解析、文件 IO、Raft 状态转换或业务流程。
  - 验收标准: 类型覆盖 Pending/Committed/Deleted、request_id、object manifest、tombstone 和幂等结果；不修改现有 `Command` 结构和 KV command 语义。

- [x] T005 Implement metadata command codec in modules/raft/common/metadata_command.cpp
  - 目标: 实现 metadata command 的序列化、反序列化、fingerprint 和字段校验。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/common/metadata_command.h`, `modules/raft/common/command.cpp`, `specs/005-strong-consistency-metadata-layer/api.md`
  - 允许修改文件: `modules/raft/common/metadata_command.cpp`
  - 实现要求: 不复用 `SET|key|value` / `DEL|key|` 作为 metadata 格式；必须拒绝大 payload、缺失 request_id、空 object_key、非法 chunk_size/chunk_count。
  - 验收标准: codec 可区分 create/commit/delete；同一 request_id 不同 fingerprint 可被上层识别为 idempotency conflict；不破坏 `command.cpp`。

- [x] T006 [P] Add metadata status mapping contract in modules/raft/common/metadata_result.h
  - 目标: 定义 metadata API 的结果码和响应摘要类型。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/api.md`, `modules/raft/common/AGENTS.md`
  - 允许修改文件: `modules/raft/common/metadata_result.h`
  - 实现要求: 覆盖 `OK`, `NOT_LEADER`, `INVALID_ARGUMENT`, `NOT_FOUND`, `IDEMPOTENT_REPLAY`, `IDEMPOTENCY_CONFLICT`, `STATE_CONFLICT`, `INTERNAL_ERROR`, `TIMEOUT`。
  - 验收标准: 后续状态机、service 和 client 可共享结果语义；不引入 gRPC/protobuf 依赖到 `common`。

- [x] T007 [P] Add metadata model unit tests in tests/metadata_command_test.cpp
  - 目标: 验证 metadata 数据模型和 command codec 的基础行为。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/common/metadata_command.h`, `modules/raft/common/metadata_result.h`, `specs/005-strong-consistency-metadata-layer/data-model.md`
  - 允许修改文件: `tests/metadata_command_test.cpp`
  - 实现要求: 若执行时 `NOTREAD.md` 仍禁止读取 `tests/**`，不得读取既有测试；只能新增该测试文件并等待后续明确 wiring 任务处理。
  - 验收标准: 覆盖合法 create/commit/delete、缺失 request_id、payload 超限、fingerprint conflict、mock_locations 解析；测试文件不依赖真实 StorageNode 或真实文件。

- [x] T008 Prepare build wiring for metadata common tests in tests/CMakeLists.txt
  - 目标: 将 metadata command 单测接入构建。
  - 允许读取文件: `NOTREAD.md`, `tests/CMakeLists.txt`, `CMakeLists.txt`
  - 允许修改文件: `tests/CMakeLists.txt`
  - 实现要求: 只添加 `metadata_command_test.cpp` 的最小 target/wiring；不得改现有测试语义或跳过失败测试。
  - 验收标准: Linux/Windows 生成器均能识别新测试 target；若 `NOTREAD.md` 禁止读取 `tests/CMakeLists.txt`，该任务必须暂停并请求许可。

**Checkpoint**: Foundation ready；完成后可以并行推进 US1、US2、US3、US4 的部分任务，但状态机核心应优先完成。

---

## Phase 3: User Story 1 - 提交强一致元数据记录 (Priority: P1) MVP

**Goal**: 支持 create -> pending invisible -> commit -> committed visible 的最小强一致元数据闭环。  
**Independent Test**: 用状态机或 Metadata Client 创建模拟对象，确认 create 后 Head/List 不可见，commit 后 Head/List 可见，并验证重复 create/commit 的 request_id 幂等。

- [ ] T009 [P] [US1] Add state machine API declarations in modules/raft/state_machine/metadata_state_machine.h
  - 目标: 定义 `StrongConsistencyMetadataStateMachine` 的公共接口。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/AGENTS.md`, `modules/raft/state_machine/state_machine.h`, `modules/raft/common/metadata_command.h`, `modules/raft/common/metadata_result.h`, `specs/005-strong-consistency-metadata-layer/plan.md`
  - 允许修改文件: `modules/raft/state_machine/metadata_state_machine.h`
  - 实现要求: 只声明 `Apply`, `SaveSnapshot`, `LoadSnapshot`, `HeadMetadataRecord`, `ListMetadataRecords` 和必要查询类型；复杂逻辑放 `.cpp`。
  - 验收标准: 接口继承或兼容 `IStateMachine` 边界；不修改 `KvStateMachine` 行为；头文件不包含复杂业务逻辑。

- [ ] T010 [US1] Implement create and commit state transitions in modules/raft/state_machine/metadata_state_machine.cpp
  - 目标: 实现 `NeverCreated -> Pending -> Committed` 状态转换和 committed-only visibility。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/metadata_state_machine.h`, `modules/raft/state_machine/state_machine.cpp`, `modules/raft/common/metadata_command.h`, `modules/raft/common/metadata_result.h`
  - 允许修改文件: `modules/raft/state_machine/metadata_state_machine.cpp`
  - 实现要求: `CreateMetadataRecord` 只产生 Pending；`CommitMetadataRecord` 才让 Head/List 可见；Pending 不得被 Head/List 返回。
  - 验收标准: VM-001、VM-002、VM-006 对应状态机行为可通过单测验证；不访问真实文件、不触碰 Raft node/replication/storage。

- [ ] T011 [US1] Implement request_id replay table for create and commit in modules/raft/state_machine/metadata_state_machine.cpp
  - 目标: 实现 create/commit 的幂等重试和 idempotency conflict。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/metadata_state_machine.h`, `modules/raft/common/metadata_command.h`, `specs/005-strong-consistency-metadata-layer/api.md`
  - 允许修改文件: `modules/raft/state_machine/metadata_state_machine.cpp`
  - 实现要求: 同一 request_id 同 fingerprint 返回等价结果；同一 request_id 不同 fingerprint 返回 `IDEMPOTENCY_CONFLICT`；重复 commit 不产生重复可见记录。
  - 验收标准: VM-003、VM-004、VM-005 通过；幂等表结果包含 request_id、operation、object_key、fingerprint、result_state、log_index。

- [ ] T012 [P] [US1] Add metadata state machine unit tests in tests/metadata_state_machine_test.cpp
  - 目标: 覆盖 US1 的状态机级别 MVP 测试。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/metadata_state_machine.h`, `modules/raft/common/metadata_command.h`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: `tests/metadata_state_machine_test.cpp`
  - 实现要求: 若 `NOTREAD.md` 仍禁止读取 `tests/**`，不得读取既有测试；新增独立测试文件即可。
  - 验收标准: 测试覆盖 create pending invisible、commit visible、duplicate create、idempotency conflict、missing pending commit。

- [ ] T013 [US1] Add metadata state machine test wiring in tests/CMakeLists.txt
  - 目标: 将 `metadata_state_machine_test.cpp` 接入构建。
  - 允许读取文件: `NOTREAD.md`, `tests/CMakeLists.txt`
  - 允许修改文件: `tests/CMakeLists.txt`
  - 实现要求: 只做最小 test target 追加；不得跳过或删除已有测试。
  - 验收标准: 新测试 target 可被 CTest 发现；若读取 `tests/CMakeLists.txt` 被禁止则暂停。

- [ ] T014 [P] [US1] Add MetadataService contract in proto/raft.proto
  - 目标: 规划并实现 Metadata API 的 protobuf 契约扩展。
  - 允许读取文件: `NOTREAD.md`, `proto/AGENTS.md`, `proto/raft.proto`, `specs/005-strong-consistency-metadata-layer/api.md`
  - 允许修改文件: `proto/raft.proto`
  - 实现要求: 新增 `MetadataService` 和相关 message/enum；不得修改 `RaftService` 语义；不得复用 KV 状态码造成语义歧义。
  - 验收标准: 契约包含 Create/Commit/Delete/Head/List、leader hint、term、log_index、request_id、state 和细分错误码。

- [ ] T015 [US1] Implement metadata service create/commit/head/list adapter in modules/raft/service/metadata_service_impl.cpp
  - 目标: 将 MetadataService create/commit 写请求转换为 Raft metadata proposal，将 Head/List 读请求映射到 committed-only 查询。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/service/AGENTS.md`, `modules/raft/service/kv_service_impl.cpp`, `modules/raft/node/raft_node.h`, `modules/raft/common/metadata_command.h`, `proto/raft.proto`
  - 允许修改文件: `modules/raft/service/metadata_service_impl.cpp`
  - 实现要求: service 层只做适配、校验和响应填充；不持有 metadata 生命周期状态；not leader 返回 leader hint；写成功返回 term/log_index。
  - 验收标准: Create/Commit/Head/List 的响应码符合 `api.md`；不改 Raft 内核逻辑。

- [ ] T016 [P] [US1] Add metadata service declaration in modules/raft/service/metadata_service_impl.h
  - 目标: 声明 MetadataService gRPC 适配类。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/service/AGENTS.md`, `modules/raft/service/kv_service_impl.h`, `proto/raft.proto`
  - 允许修改文件: `modules/raft/service/metadata_service_impl.h`
  - 实现要求: `.h` 只放类声明和方法签名；不写业务状态转换。
  - 验收标准: `.cpp` 可引用该声明；不改变 `KvServiceImpl` 的公开行为。

- [ ] T017 [US1] Wire MetadataService into server startup in apps/main.cpp
  - 目标: 在 demo 服务端注册 MetadataService。
  - 允许读取文件: `NOTREAD.md`, `apps/AGENTS.md`, `apps/main.cpp`, `modules/raft/service/metadata_service_impl.h`
  - 允许修改文件: `apps/main.cpp`
  - 实现要求: 只做服务注册和必要 include；不在入口层写业务逻辑；不改变 Raft 节点启动参数语义。
  - 验收标准: 原 KV service 能继续注册；MetadataService 可在同一节点上对外提供元数据 API。

**Checkpoint**: US1 MVP 完成后，应能独立验证 create/commit/head/list 和 committed-only visibility。

---

## Phase 4: User Story 2 - 删除与 tombstone 可恢复 (Priority: P2)

**Goal**: 支持 Committed -> Deleted tombstone，删除后 Head/List 不可见，并在 snapshot/restart 后恢复 committed metadata 与 tombstone。  
**Independent Test**: create + commit + delete 后 Head/List 不可见；snapshot/restart 后仍不可见；相同 delete request_id 重试幂等。

- [ ] T018 [US2] Implement delete tombstone transition in modules/raft/state_machine/metadata_state_machine.cpp
  - 目标: 实现 `Committed -> Deleted` tombstone 语义。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/metadata_state_machine.h`, `modules/raft/common/metadata_command.h`, `specs/005-strong-consistency-metadata-layer/data-model.md`
  - 允许修改文件: `modules/raft/state_machine/metadata_state_machine.cpp`
  - 实现要求: Delete 只允许作用于 Committed；Deleted 从 Head/List 隐藏；Pending delete 返回 `STATE_CONFLICT`；never-created delete 返回 `NOT_FOUND`。
  - 验收标准: VM-007、VM-009 通过；删除不会物理丢失 tombstone 事实。

- [ ] T019 [US2] Extend idempotency replay for delete in modules/raft/state_machine/metadata_state_machine.cpp
  - 目标: 支持 delete request_id 重试和旧请求复活防护。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/metadata_state_machine.h`, `modules/raft/common/metadata_command.h`, `specs/005-strong-consistency-metadata-layer/api.md`
  - 允许修改文件: `modules/raft/state_machine/metadata_state_machine.cpp`
  - 实现要求: 相同 delete request_id 返回幂等结果；旧 create/commit 不得使 Deleted 重新变为 Committed；不同 request_id 删除已 Deleted 的对外结果必须固定。
  - 验收标准: VM-008、VM-010 通过；幂等表和 tombstone 状态一致。

- [ ] T020 [US2] Implement metadata snapshot save/load in modules/raft/state_machine/metadata_state_machine.cpp
  - 目标: 保存并加载 committed metadata、tombstone 和必要幂等表。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/state_machine.cpp`, `modules/raft/state_machine/metadata_state_machine.h`, `specs/005-strong-consistency-metadata-layer/plan.md`
  - 允许修改文件: `modules/raft/state_machine/metadata_state_machine.cpp`
  - 实现要求: 新 metadata snapshot 格式必须有 magic/version；不得修改 KV snapshot 格式；required durability operation 不允许 no-op 成功。
  - 验收标准: Save/Load 后 Committed 可见、Deleted 不可见、Pending 不外部可见；损坏/版本不匹配返回明确错误。

- [ ] T021 [P] [US2] Add tombstone and restart unit tests in tests/metadata_snapshot_test.cpp
  - 目标: 覆盖 tombstone、snapshot/restart 和 Pending 恢复不可见。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/state_machine/metadata_state_machine.h`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: `tests/metadata_snapshot_test.cpp`
  - 实现要求: 若 `NOTREAD.md` 仍禁止读取 `tests/**`，不得读取既有测试；新增独立测试文件即可。
  - 验收标准: 覆盖 VM-011、VM-012、VM-013；测试使用测试临时目录，不读取运行数据目录。

- [ ] T022 [US2] Add DeleteMetadataRecord adapter in modules/raft/service/metadata_service_impl.cpp
  - 目标: 将 DeleteMetadataRecord API 接入服务层。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/service/metadata_service_impl.cpp`, `proto/raft.proto`, `specs/005-strong-consistency-metadata-layer/api.md`
  - 允许修改文件: `modules/raft/service/metadata_service_impl.cpp`
  - 实现要求: delete 写请求通过 Raft proposal；响应包含 request_id、state、term、log_index、leader hint；不在 service 层保存 tombstone。
  - 验收标准: Delete 成功、Delete retry、Delete Pending conflict、Delete unknown 的响应码符合 `api.md`。

**Checkpoint**: US2 完成后，删除和 restart recovery 可独立验证。

---

## Phase 5: User Story 3 - Leader failover 后验证 committed metadata 不丢失 (Priority: P3)

**Goal**: 使用现有 Raft failover 能力验证 committed metadata 不丢失，Pending 不暴露，request_id 可跨 leader 重试。  
**Independent Test**: 提交若干 metadata 后切换 leader，新 leader 上 Head/List 仍可见 committed metadata；Pending 不可见；同 request_id retry 不重复。

- [ ] T023 [US3] Add metadata integration test for leader failover in tests/metadata_failover_test.cpp
  - 目标: 验证 leader failover 后 committed metadata 可见且 Pending 不可见。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`, `proto/raft.proto`
  - 允许修改文件: `tests/metadata_failover_test.cpp`
  - 实现要求: 若执行时需要复用既有集群测试工具但 `NOTREAD.md` 禁止读取 `tests/**`，必须暂停请求许可；不得复制大段旧测试。
  - 验收标准: 覆盖 VM-014、VM-015；失败时记录失败测试名、关键断言、最后 50 行日志和完整日志路径。

- [ ] T024 [US3] Ensure metadata reads are leader-safe in modules/raft/service/metadata_service_impl.cpp
  - 目标: 保证 Head/List 读路径不从不安全 follower 暴露 stale metadata。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/service/metadata_service_impl.cpp`, `modules/raft/service/kv_service_impl.cpp`, `modules/raft/node/raft_node.h`
  - 允许修改文件: `modules/raft/service/metadata_service_impl.cpp`
  - 实现要求: 默认只允许 leader serving Head/List 或使用已有明确线性一致读路径；not leader 返回 leader hint。
  - 验收标准: failover 期间 follower Head/List 不错误返回 committed 或 Pending 状态；响应码可诊断。

- [ ] T025 [US3] Add failover retry scenario to Metadata Client in apps/raft_metadata_client.cpp
  - 目标: 支持用户用同一 request_id 在 failover 后重试 commit/delete。
  - 允许读取文件: `NOTREAD.md`, `apps/AGENTS.md`, `apps/raft_metadata_client.cpp`, `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 允许修改文件: `apps/raft_metadata_client.cpp`
  - 实现要求: `NOT_LEADER` 或 `TIMEOUT` 后不得生成新 request_id；可显示 leader hint；不读取真实文件。
  - 验收标准: VM-016 可通过客户端手动或脚本验证；输出包含 request_id、leader_id、leader_address、term、log_index。

- [ ] T026 [P] [US3] Add client failover flow documentation in specs/005-strong-consistency-metadata-layer/client-design.md
  - 目标: 记录 leader failover retry 的客户端使用流程。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/client-design.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 实现要求: 只更新文档，不修改源码；明确同 request_id retry 和读后写验证命令。
  - 验收标准: 文档中有 failover 后 commit retry、delete retry、Head/List 验证步骤。

**Checkpoint**: US3 完成后，failover safety 可独立验证。

---

## Phase 6: User Story 4 - 为 StorageNode 和 ChunkStore 保留扩展边界 (Priority: P4)

**Goal**: 明确并验证当前阶段只处理 metadata control plane，未来 StorageNode/ChunkStore 可接入但不影响 committed-only visibility。  
**Independent Test**: 使用不存在的 mock_locations 仍可 create/commit/head/list；系统不要求真实 chunk 文件或 StorageNode。

- [ ] T027 [US4] Implement manifest boundary validation in modules/raft/common/metadata_command.cpp
  - 目标: 确保 manifest 只描述模拟位置和 metadata payload，不接收真实大文件数据。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/common/metadata_command.cpp`, `modules/raft/common/metadata_command.h`, `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 允许修改文件: `modules/raft/common/metadata_command.cpp`
  - 实现要求: 校验 object_size/chunk_size/chunk_count/checksum/mock_locations；payload 有上限；不访问本地文件路径、不检查 StorageNode。
  - 验收标准: VM-017、VM-018、VM-020 对 codec 层通过；无真实文件 IO。

- [ ] T028 [US4] Add manifest boundary tests in tests/metadata_manifest_test.cpp
  - 目标: 验证模拟 manifest 和 payload 边界。
  - 允许读取文件: `NOTREAD.md`, `modules/raft/common/metadata_command.h`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: `tests/metadata_manifest_test.cpp`
  - 实现要求: 不读取既有 tests；测试 mock_locations 指向不存在节点也应接受，payload 超限应拒绝。
  - 验收标准: 覆盖合法 manifest、非法 chunk_size、chunk_count 不匹配、checksum 缺失、payload 超限、mock StorageNode 不存在。

- [ ] T029 [US4] Add Metadata Client create generator in apps/raft_metadata_client.cpp
  - 目标: 实现客户端模拟 object_key、object_size、chunk_size、chunk_count、checksum、mock_locations、payload 的生成和解析。
  - 允许读取文件: `NOTREAD.md`, `apps/AGENTS.md`, `apps/raft_kv_client.cpp`, `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 允许修改文件: `apps/raft_metadata_client.cpp`
  - 实现要求: 不打开真实文件；checksum 是 mock 字符串；chunk_count 可自动计算；允许用户显式传入 mock_locations。
  - 验收标准: 客户端能发起 create 请求；输出明确 payload 是 metadata-only；不需要 StorageNode。

- [ ] T030 [US4] Document future StorageNode boundary in specs/005-strong-consistency-metadata-layer/plan.md
  - 目标: 更新文档，说明当前 metadata manifest 与未来 StorageNode/ChunkStore 的扩展点。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/plan.md`, `specs/005-strong-consistency-metadata-layer/data-model.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/plan.md`
  - 实现要求: 只更新文档；不得新增 StorageNode 实现任务到当前阶段。
  - 验收标准: 文档明确未来只消费 object_key、chunk manifest、checksum、location 引用，不改变当前 committed-only visibility 和 tombstone 语义。

**Checkpoint**: US4 完成后，当前阶段与未来数据面边界清晰，且 mock_locations 不要求真实节点。

---

## Phase 7: Metadata Client Completion

**Purpose**: 将客户端从 KV 操作演进为完整 Metadata Client，但不实现真实文件传输。

- [ ] T031 Implement Metadata Client command dispatcher in apps/raft_metadata_client.cpp
  - 目标: 支持 create、commit、delete、head、list 子命令。
  - 允许读取文件: `NOTREAD.md`, `apps/AGENTS.md`, `apps/raft_kv_client.cpp`, `proto/raft.proto`, `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 允许修改文件: `apps/raft_metadata_client.cpp`
  - 实现要求: 入口层只解析参数和发起 RPC；不写业务状态机；不读取真实文件。
  - 验收标准: 每个子命令参数错误返回非 0 并打印 usage；RPC 成功输出稳定字段。

- [ ] T032 Add client read-after-write verification mode in apps/raft_metadata_client.cpp
  - 目标: 支持 create 后验证不可见、commit 后验证可见、delete 后验证不可见。
  - 允许读取文件: `NOTREAD.md`, `apps/raft_metadata_client.cpp`, `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 允许修改文件: `apps/raft_metadata_client.cpp`
  - 实现要求: 验证模式只调用 Metadata API；不访问 Raft 内部日志或 snapshot；失败时输出期望与实际状态。
  - 验收标准: 可覆盖 VM-001、VM-002、VM-007 的客户端基本流程。

- [ ] T033 Add client target wiring in CMakeLists.txt
  - 目标: 增加 Metadata Client 可执行目标。
  - 允许读取文件: `NOTREAD.md`, `CMakeLists.txt`, `apps/raft_metadata_client.cpp`
  - 允许修改文件: `CMakeLists.txt`
  - 实现要求: 保持现有 target 名称不变；新增 target 不破坏 `raft_kv_client`；生成 protobuf/gRPC include 方式保持一致。
  - 验收标准: Linux 和 Windows CMake configure/build 均能生成 metadata client target。

- [ ] T034 Add client scenario tests in tests/metadata_client_scenario_test.cpp
  - 目标: 覆盖 Metadata Client 的模拟日志和读后写验证。
  - 允许读取文件: `NOTREAD.md`, `apps/raft_metadata_client.cpp`, `specs/005-strong-consistency-metadata-layer/client-design.md`
  - 允许修改文件: `tests/metadata_client_scenario_test.cpp`
  - 实现要求: 若读取 tests 禁止，则只新增独立测试文件；不依赖真实 StorageNode 或真实文件。
  - 验收标准: 测试覆盖 create/commit/head/list/delete、重复请求、payload boundary 和 mock_locations。

---

## Phase 8: Documentation & Boundary Polish

**Purpose**: 收口文档、阶段边界和任务可追踪性；不修改业务逻辑。

- [ ] T035 [P] Update API notes in specs/005-strong-consistency-metadata-layer/api.md
  - 目标: 将实现中固定的状态码、重复请求结果和 deleted-again 行为回填到 API 文档。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/api.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/api.md`
  - 实现要求: 只更新文档；不得改变源码。
  - 验收标准: API 文档不再存在“实现阶段需固定”的开放语句。

- [ ] T036 [P] Update data model notes in specs/005-strong-consistency-metadata-layer/data-model.md
  - 目标: 回填最终字段约束、payload 上限和 tombstone 保留策略。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/data-model.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/data-model.md`
  - 实现要求: 只更新文档；不得新增 StorageNode 数据模型为当前阶段任务。
  - 验收标准: 数据模型与实现任务中的字段和状态转换一致。

- [ ] T037 [P] Update validation matrix status in specs/005-strong-consistency-metadata-layer/validation-matrix.md
  - 目标: 将已实现验证与待验证项映射到具体测试目标和平台。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 实现要求: 只更新文档；保留 Linux/Windows 分平台验证项。
  - 验收标准: VM-001 到 VM-020 均能追踪到单测、集成测试或客户端流程验证。

---

## Phase 9: Linux Validation

**Purpose**: 在 Linux 平台验证构建、单测/集成测试和 Metadata Client 基本流程。

- [ ] T038 Run Linux configure and build validation with cmake presets
  - 目标: 确认 Linux 平台可配置和构建。
  - 允许读取文件: `NOTREAD.md`, `CMakeLists.txt`, `CMakePresets.json`, `specs/005-strong-consistency-metadata-layer/tasks.md`
  - 允许修改文件: 无
  - 实现要求: 执行 `cmake --preset debug-ninja-low-parallel` 和 `cmake --build --preset debug-ninja-low-parallel`；不得读取 build 产物内容，除非命令失败时只摘录必要错误摘要。
  - 验收标准: 构建 PASS；失败时记录命令、失败分类、关键编译错误、最后 50 行日志和完整日志路径。

- [ ] T039 Run Linux unit and integration validation with CTest
  - 目标: 验证 metadata common/state_machine/service/client 相关测试。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: 无
  - 实现要求: 建议执行 `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build --output-on-failure -R "Metadata"`；如果实际测试名不同，使用明确 metadata test regex；不得全文输出日志。
  - 验收标准: PASS；失败时只输出失败测试名、关键断言、失败分类、最后 50 行日志和完整日志文件路径。

- [ ] T040 Run Linux Metadata Client basic flow validation
  - 目标: 验证客户端 create、head、commit、list、delete、head 基本流程。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/client-design.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: 无
  - 实现要求: 启动最小 demo 集群或已有本地节点后执行 Metadata Client；使用固定 request_id 和 mock manifest；不读取真实文件、不写真实 chunk。
  - 验收标准: create 后 head/list not found；commit 后 head/list found；delete 后 head/list not found；失败时记录客户端命令、响应 code/message、leader hint、term/log_index 和日志路径。

---

## Phase 10: Windows Validation

**Purpose**: 在 Windows 平台验证跨平台构建、测试和 Metadata Client 基本流程。

- [ ] T041 Run Windows configure and build validation with CMake
  - 目标: 确认 Windows 平台可配置和构建。
  - 允许读取文件: `NOTREAD.md`, `CMakeLists.txt`, `CMakePresets.json`, `specs/005-strong-consistency-metadata-layer/tasks.md`
  - 允许修改文件: 无
  - 实现要求: 使用项目已有 Windows 兼容 CMake preset 或等价命令；优先保持 generator-agnostic；不得依赖 Linux-only shell 语义。
  - 验收标准: 构建 PASS；失败时记录命令、生成器、编译器、失败分类、关键错误、最后 50 行日志和完整日志路径。

- [ ] T042 Run Windows unit and integration validation with CTest
  - 目标: 验证 Windows 平台 metadata 相关单测和集成测试。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: 无
  - 实现要求: 使用 Windows 可用的 `ctest --test-dir <build-dir> --output-on-failure -R "Metadata"`；不得使用 Linux-only 环境变量作为唯一入口。
  - 验收标准: PASS；失败时记录失败测试名、关键断言、失败分类、最后 50 行日志和完整日志文件路径。

- [ ] T043 Run Windows Metadata Client basic flow validation
  - 目标: 验证 Windows 平台 Metadata Client 的 create、head、commit、list、delete、head 流程。
  - 允许读取文件: `NOTREAD.md`, `specs/005-strong-consistency-metadata-layer/client-design.md`, `specs/005-strong-consistency-metadata-layer/validation-matrix.md`
  - 允许修改文件: 无
  - 实现要求: 使用 Windows 可执行文件路径和 Windows shell 兼容参数；不依赖 bash；不读取真实文件、不写真实 chunk。
  - 验收标准: 与 Linux 基本流程同等结果；失败时记录客户端命令、响应 code/message、leader hint、term/log_index、Windows 版本/生成器信息和完整日志路径。

---

## Dependencies & Execution Order

### Phase Dependencies

- Phase 1 Setup 无依赖，可立即执行。
- Phase 2 Foundational 依赖 Phase 1，阻塞所有 user story。
- US1 是 MVP，依赖 Phase 2。
- US2 依赖 US1 的状态机 create/commit 基础。
- US3 依赖 US1，部分 failover tombstone 验证依赖 US2。
- US4 可在 US1 后并行推进，但 manifest boundary 的验证依赖 command codec。
- Phase 7 Metadata Client Completion 依赖 API/Service 基础和 US1，delete/failover 子流程分别依赖 US2/US3。
- Phase 8 Documentation 依赖对应实现任务。
- Phase 9 Linux Validation 和 Phase 10 Windows Validation 必须在目标实现与测试 wiring 完成后执行。

### User Story Counts

- US1: T009-T017，共 9 个任务。
- US2: T018-T022，共 5 个任务。
- US3: T023-T026，共 4 个任务。
- US4: T027-T030，共 4 个任务。

### Parallel Opportunities

- T002 和 T003 可并行。
- T006 和 T007 可在 T004/T005 接口稳定后并行。
- T009 与 T014/T016 可并行，但 T015 依赖服务契约和状态机写路径。
- T021 可与 T022 并行，但都依赖 T018/T019 的语义。
- T026 可与 T023/T024/T025 并行。
- T035、T036、T037 可并行。
- Linux 与 Windows 验证不可互相替代，应分别执行。

## Implementation Strategy

### MVP First

1. 完成 T001-T008。
2. 完成 US1 的 T009-T017。
3. 只验证 create -> pending invisible -> commit -> committed visible 和 create/commit 幂等。
4. MVP 通过后再进入 tombstone、restart、failover 和 client 完整化。

### Incremental Delivery

1. Foundation: metadata command + 数据模型 + 基础测试。
2. US1: create/commit/head/list。
3. US2: delete/tombstone/snapshot/restart。
4. US3: leader failover retry 和安全读。
5. US4: StorageNode/ChunkStore 边界和 manifest/payload 防线。
6. Client: 完整 CLI 和读后写验证。
7. Linux/Windows 双平台验证。

### Rollback Strategy

- metadata command 与 KV command 分离，必要时可回滚 metadata 文件而不影响 KV baseline。
- MetadataService 新增，不修改 RaftService；回滚时移除 service 注册和 proto 扩展即可。
- Metadata Client 独立于 `raft_kv_client`，回滚不影响现有 KV client。
- metadata snapshot 格式独立于 KV snapshot；不得修改 KV snapshot magic/version。
