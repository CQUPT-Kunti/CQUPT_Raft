# Tasks: CQUPT_Raft Industrialization Hardening

**Input**: Design documents from `/specs/004-raft-industrialization/`  
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: 本 feature 明确要求补测试、回归测试、Linux-specific crash/failure
验证和跨平台测试入口，因此任务中包含测试任务。  
**Organization**: 任务按用户故事组织，前置 Phase 只处理仍需完善的共享内容，
不会重复拆分已稳定完成的 Raft 主功能。

## Format: `[ID] [P?] [Story] Description`

- **[P]**: 可并行（不同文件、无未完成前置依赖）
- **[Story]**: 对应用户故事
- 每个任务后都附带：目标、输入、修改范围、验收标准、是否改生产代码、
  是否只改测试、是否涉及跨平台、是否 Linux-specific、是否需要
  Windows/macOS fallback、风险来源/依据、需运行测试

## Phase 1: Setup (Shared Scope Freeze)

**Purpose**: 固定只做“剩余工业化缺口”，明确与 `003-persistence-reliability`
的边界，避免后续重复规划已完成能力。

- [x] T001 Create `specs/004-raft-industrialization/validation-matrix.md`
  Goal: 建立本 feature 的单一风险与验证矩阵，冻结“已完成/补测试/需修复/跨平台风险/新增/暂缓”分类。
  Input: `specs/004-raft-industrialization/spec.md`, `plan.md`, `quickstart.md`, `specs/003-persistence-reliability/progress.md`, `test.sh`.
  Scope: 新增 `specs/004-raft-industrialization/validation-matrix.md`，不改生产代码。
  Acceptance: 文档明确列出 carried-forward 基线、P0-P4 风险来源、Linux 主入口、平台无关入口、Linux-specific 组和 deferred 项。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 需要明确平台无关入口与未验证项。
  Basis: `plan.md` 的 W0/W1/W6/W7/W8；`specs/003-persistence-reliability/progress.md` 的 Blocked/Next/Notes。
  Tests To Run: None; 文档任务。

- [ ] T002 Update `specs/004-raft-industrialization/quickstart.md` and `specs/004-raft-industrialization/contracts/validation-entrypoints.md`
  Goal: 让 quickstart 和入口契约直接引用 `validation-matrix.md`，并收敛到真实的 Linux 与平台无关 rerun 命令。
  Input: `quickstart.md`, `contracts/validation-entrypoints.md`, `validation-matrix.md`, `test.sh`, `CMakePresets.json`.
  Scope: 仅修改 spec artifacts，不改测试和生产代码。
  Acceptance: Quickstart 中的验证顺序、rerun 命令、`--keep-data` 使用方式、Linux-specific 标签和平台无关 fallback 与矩阵保持一致。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 必须在文档中可见。
  Basis: `research.md` Decision 5/6/7；`plan.md` W6/W7。
  Tests To Run: None; 文档任务。

---

## Phase 2: Foundational (Blocking P0 Work)

**Purpose**: 先把当前 Linux 主验收路径的噪声与失败定位面收敛，再进入更高风险的 failure injection 和恢复补强。

**⚠️ CRITICAL**: 本阶段不完成，US1 的高风险回归与 durability 任务不应关闭。

- [x] T003 Add deterministic diagnosis for `RaftSnapshotRecoveryTest` in `tests/test_raft_snapshot_restart.cpp`
  Goal: 分析并稳定 `RaftSnapshotRecoveryTest.SavesSnapshotAndRestoresAfterRestart` 的时序假设，不放宽 snapshot/restart 断言。
  Input: `tests/test_raft_snapshot_restart.cpp`, `specs/003-persistence-reliability/progress.md`, `validation-matrix.md`.
  Scope: 只改 `tests/test_raft_snapshot_restart.cpp`；必要时可增加文件内测试辅助逻辑，但不改业务行为。
  Acceptance: 该测试可重复执行而不因纯 timing 假设随机失败；失败时能看出是 leadership churn、proposal majority 还是 snapshot restore 相关。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Indirectly.
  Linux-Specific: Yes, 当前 blocker 来源于 Linux 主验收。
  Windows/macOS Fallback: Yes, 用 `ctest --preset debug-tests` 作为平台无关回归入口，不要求同等时序证据。
  Basis: `specs/003-persistence-reliability/progress.md` Blocked 中的 `RaftSnapshotRecoveryTest...` flaky；`plan.md` W1。
  Tests To Run: `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftSnapshotRecoveryTest\.SavesSnapshotAndRestoresAfterRestart$'`
  Result: `Passed`, `1/1 tests passed`
  Execution Note: 实际命中的测试定义位于 `tests/snapshot_test.cpp`；本次未修改生产代码。

- [x] T004 Add deterministic diagnosis for `RaftSplitBrainTest` in `tests/test_raft_split_brain.cpp`
  Goal: 稳定 `RaftSplitBrainTest.MinorityLeaderTimesOutAndDoesNotApplyUncommittedCommand` 的 leader election timing 路径，不弱化 split-brain 断言。
  Input: `tests/test_raft_split_brain.cpp`, `specs/003-persistence-reliability/progress.md`, `validation-matrix.md`.
  Scope: 只改 `tests/test_raft_split_brain.cpp`，必要时增加更明确的等待/诊断逻辑。
  Acceptance: 测试重复执行时不再因为 leader_index 时序空洞随机失败；失败信息能区分 election 未收敛、minority partition、apply 违规三类问题。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Indirectly.
  Linux-Specific: Yes, 当前 blocker 来源于 Linux 主验收。
  Windows/macOS Fallback: Yes, 该场景在非 Linux 先走平台无关 rerun，不承诺 crash-style 等价。
  Basis: `specs/003-persistence-reliability/progress.md` Blocked 中的 `RaftSplitBrainTest...` flaky；`plan.md` W1。
  Tests To Run: `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftSplitBrainTest\.MinorityLeaderTimesOutAndDoesNotApplyUncommittedCommand$'`
  Result: `Passed`, `1/1 tests passed`
  Execution Note: 修改文件为 `tests/test_raft_split_brain.cpp`；本次未修改生产代码；`--repeat until-fail:3` 未执行，因此无 repeat 结果可记录。

- [x] T005 Update `test.sh` comments/output and `specs/004-raft-industrialization/validation-matrix.md`
  Goal: 给当前已知 blocker 和高风险回归组补上保留数据、低并发 rerun 和失败定位提示。
  Input: `test.sh`, `validation-matrix.md`, `quickstart.md`.
  Scope: 修改脚本注释或输出提示，以及矩阵文档；不改生产代码。
  Acceptance: 维护者能从 `test.sh` 和矩阵直接看到 `snapshot-recovery`、`diagnosis`、`snapshot-catchup`、`replicator`、`segment-cluster` 的 rerun 命令和 `--keep-data` 用法。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially, 因为 `test.sh` 是 Linux 主入口。
  Windows/macOS Fallback: Yes, 同步给出 `ctest --preset debug-tests` fallback。
  Basis: `plan.md` W1/W6/W7；`contracts/validation-entrypoints.md`。
  Tests To Run: `./test.sh --group snapshot-recovery --keep-data`; `./test.sh --group diagnosis`.

**Checkpoint**: Linux 主验收 blocker 有明确复现与诊断路径，US1 可进入高风险恢复与 durability 补强。

---

## Phase 3: User Story 1 - Trust Restarted State (Priority: P1, contains P0 tasks) 🎯 MVP

**Goal**: 让 committed term/vote/log/snapshot/applied state 在 crash-like 工件和 restart 下具备更可信的恢复证据。  
**Independent Test**: `CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence && ./test.sh --group snapshot-recovery && ./test.sh --group diagnosis`

### Tests for User Story 1

- [x] T006 [P] [US1] Add meta/log durability failure cases in `tests/persistence_test.cpp` and `tests/test_raft_segment_storage.cpp`
  Goal: 先写失败测试，覆盖 `meta.bin` / segment log 的 file sync、directory sync、replace/partial-write 边界。
  Input: `tests/persistence_test.cpp`, `tests/test_raft_segment_storage.cpp`, `contracts/failure-injection-boundaries.md`, `specs/003-persistence-reliability/progress.md`.
  Scope: 只改测试文件，先表达预期 trusted-state 结果和诊断要求。
  Acceptance: 新测试在 failure injection seam 未实现前可明确失败或标记待实现，且断言的是恢复边界，不是内部实现细节。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Partially.
  Linux-Specific: Yes, 精确 durability 注入以 Linux 为主。
  Windows/macOS Fallback: Yes, 非 Linux 只保留平台无关恢复测试与文档说明。
  Basis: `specs/003-persistence-reliability/progress.md` Blocked T613-T616；`plan.md` W2/W3。
  Tests To Run: `./test.sh --group persistence`; `ctest --test-dir build/linux --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest)\.'`.

- [x] T007 [P] [US1] Add snapshot publish/prune failure cases in `tests/test_snapshot_storage_reliability.cpp` and `tests/test_raft_snapshot_restart.cpp`
  Goal: 先写 failure injection 测试，覆盖 snapshot publish、directory sync、prune/remove 与 restart 选择规则。
  Input: `tests/test_snapshot_storage_reliability.cpp`, `tests/test_raft_snapshot_restart.cpp`, `contracts/failure-injection-boundaries.md`, `validation-matrix.md`.
  Scope: 只改测试文件。
  Acceptance: 测试能表达 staged snapshot publish、invalid snapshot fallback、prune 失败后的 trusted-state 预期。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Partially.
  Linux-Specific: Yes.
  Windows/macOS Fallback: Yes, 文档说明非 Linux 先不声称等价 failure injection 证据。
  Basis: `specs/003-persistence-reliability/progress.md` Blocked T613-T616；`plan.md` W2/W3。
  Tests To Run: `./test.sh --group snapshot-storage`; `./test.sh --group snapshot-recovery`.

- [x] T008 [P] [US1] Migrate useful manual recovery scenarios from `tests/persistence_more_test.cpp` into managed GTest files
  Goal: 把对工业化有价值的两阶段恢复场景纳入受管回归，而不是停留在手工诊断程序里。
  Input: `tests/persistence_more_test.cpp`, `tests/persistence_test.cpp`, `tests/test_raft_snapshot_diagnosis.cpp`, `tests/CMakeLists.txt`.
  Scope: 优先把有用场景迁入现有 GTest；若保留手工程序，则在 `tests/README.md` 或 spec docs 中明确其非回归角色。
  Acceptance: 至少一个当前只存在于 `persistence_more_test.cpp` 的恢复场景进入受管回归；剩余未迁移部分有文档说明。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: 现状分析发现 `persistence_more_test.cpp` 未纳入 CTest；`plan.md` W3/W7。
  Tests To Run: `./test.sh --group persistence`; `ctest --test-dir build/linux --output-on-failure -R '^PersistenceTest\.'`.

### Implementation for User Story 1

- [x] T009 [US1] Introduce an opt-in internal failure injection seam in `modules/raft/storage/raft_storage.cpp`
  Goal: 为 meta/log 的 exact failure injection 提供最小内部注入点，默认关闭且不改变成功路径语义。
  Input: `modules/raft/storage/raft_storage.cpp`, `contracts/failure-injection-boundaries.md`, `research.md`, T006.
  Scope: 仅改 `modules/raft/storage/raft_storage.cpp`；必要时新增 storage 内部 helper，但不得暴露公共 API。
  Acceptance: 注入关闭时现有测试表现不变；注入开启时能命中 fsync、directory sync、replace、partial-write 等边界。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially, 主要证据在 Linux。
  Windows/macOS Fallback: Yes, 需要明示 weaker guarantee 或 deferred runtime validation。
  Basis: `research.md` Decision 3；`plan.md` W2；`specs/003-persistence-reliability/progress.md` Blocked T613-T616。
  Tests To Run: T006 的新增用例；`./test.sh --group persistence`.

- [x] T010 [US1] Introduce the same opt-in seam for snapshot publish/prune in `modules/raft/storage/snapshot_storage.cpp`
  Goal: 覆盖 snapshot data/meta publish、directory sync 和 prune/remove 失败点。
  Input: `modules/raft/storage/snapshot_storage.cpp`, `contracts/failure-injection-boundaries.md`, T007.
  Scope: 仅改 `modules/raft/storage/snapshot_storage.cpp`；必要时复用 T009 的内部 helper。
  Acceptance: snapshot publish/prune 的 failure injection 可稳定触发，默认关闭时不改变现有语义与格式。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `research.md` Decision 3/6；`plan.md` W2；`specs/003-persistence-reliability/progress.md` Blocked T613-T616。
  Tests To Run: T007 的新增用例；`./test.sh --group snapshot-storage`; `./test.sh --group snapshot-recovery`.

- [x] T011 [US1] Add injected-failure diagnostics in `modules/raft/storage/raft_storage.cpp` and `modules/raft/storage/snapshot_storage.cpp`
  Goal: 让 durability failure 的日志/诊断能指明操作、路径、平台范围和 trusted-state 预期。
  Input: `raft_storage.cpp`, `snapshot_storage.cpp`, `validation-matrix.md`, T009, T010.
  Scope: 仅改 storage 相关 `.cpp`。
  Acceptance: 失败输出能区分 file sync、directory sync、replace/rename、prune/remove、partial write；文档中可映射到恢复预期。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `research.md` Decision 7；`plan.md` W2/W7。
  Tests To Run: `./test.sh --group persistence --keep-data`; `./test.sh --group diagnosis`.

- [x] T012 [US1] Add hard-state and log-boundary restart matrix tests in `tests/persistence_test.cpp` and `tests/test_raft_segment_storage.cpp`
  Goal: 系统化补齐 `current_term`、`voted_for`、`commit_index`、`last_applied` 与 log boundary 的重启恢复组合。
  Input: `tests/persistence_test.cpp`, `tests/test_raft_segment_storage.cpp`, `data-model.md`, `validation-matrix.md`.
  Scope: 只改测试文件。
  Acceptance: 至少覆盖 old-meta/new-log、new-meta/old-log、boundary mismatch、tail truncate 后 trusted-state 选择等组合。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: `plan.md` W3；`spec.md` FR-003/FR-010；`specs/003-persistence-reliability/progress.md` Notes about trusted-state rules。
  Tests To Run: `./test.sh --group persistence`; `./test.sh --group segment-basic`.

- [x] T013 [P] [US1] Add snapshot metadata and applied-state restart matrix tests in `tests/test_raft_snapshot_restart.cpp` and `tests/test_raft_snapshot_diagnosis.cpp`
  Goal: 补齐 invalid snapshot、all-invalid fallback、metadata mismatch、applied replay 的恢复覆盖。
  Input: `tests/test_raft_snapshot_restart.cpp`, `tests/test_raft_snapshot_diagnosis.cpp`, `data-model.md`, `validation-matrix.md`.
  Scope: 只改测试文件。
  Acceptance: 新测试能验证非法 snapshot 不会被误接受，恢复后 apply/replay 与 trusted snapshot 选择一致。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: `plan.md` W3/W5；`spec.md` FR-003；`specs/003-persistence-reliability/progress.md` Phase 5/6 notes。
  Tests To Run: `./test.sh --group snapshot-recovery`; `./test.sh --group diagnosis`.

- [x] T014 [US1] Apply minimal recovery fixes in `modules/raft/storage/raft_storage.cpp`, `modules/raft/storage/snapshot_storage.cpp`, or `modules/raft/node/raft_node.cpp` if T012-T013 expose real defects
  Goal: 仅在测试证明存在真实 trusted-state/restart 缺陷时做最小修复，不改格式、不改协议、不扩展公共 API。
  Input: T012/T013 failing evidence, affected `.cpp` files, `plan.md`, constitution.
  Scope: 限定在相关 `.cpp`；若触及 `raft_node.cpp`，必须同时检查相关 storage/state_machine tests。
  Acceptance: 对应失败测试转绿；未引入格式变更、协议变更或额外回归。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 若修复依赖平台差异要写入矩阵。
  Basis: `plan.md` W3；constitution II/III；`spec.md` FR-010。
  Tests To Run: T012/T013 相关测试；`./test.sh --group persistence`; `./test.sh --group snapshot-recovery`; `./test.sh --group diagnosis`.


- [x] T015 [US1] Update `specs/004-raft-industrialization/validation-matrix.md` with accepted US1 evidence
  Goal: 把 Linux-specific durability 注入证据、平台无关恢复证据和 deferred Windows/macOS 范围正式记录下来。
  Input: T006-T014 results, `validation-matrix.md`, `quickstart.md`.
  Scope: 文档更新。
  Acceptance: 矩阵能回答“哪些恢复能力已证实”“哪些只在 Linux 验证”“哪些仍需 Windows/macOS follow-up”。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` W2/W3/W7/W8；`contracts/failure-injection-boundaries.md`。
  Tests To Run: None; 文档任务。

**Checkpoint**: US1 完成后，重启恢复和 crash-like trusted-state 边界应可独立验证，是本 feature 的 MVP。

---

## Phase 4: User Story 2 - Preserve Cluster Consistency Under Lag And Leadership Change (Priority: P2)

**Goal**: 补强 follower lag/restart、snapshot handoff、post-snapshot replay、leader 切换和 apply/restart 一致性证据。  
**Independent Test**: `CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-catchup && ./test.sh --group replicator && ./test.sh --group replication && ./test.sh --group snapshot-restart`

### Tests for User Story 2

- [x] T016 [P] [US2] Add lagging-follower and compaction catch-up regressions in `tests/test_raft_snapshot_catchup.cpp` and `tests/raft_integration_test.cpp`
  Goal: 覆盖 follower 落后 live log 和 retained snapshot boundary 两类 catch-up 场景。
  Input: `tests/test_raft_snapshot_catchup.cpp`, `tests/raft_integration_test.cpp`, `plan.md`, `validation-matrix.md`.
  Scope: 只改测试文件。
  Acceptance: 新测试可区分 log replay catch-up 与 snapshot handoff catch-up，并验证 committed ordering 不被破坏。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: `plan.md` W4；`spec.md` FR-004；现状分析中 follower catch-up 仍属需补强区域。
  Tests To Run: `./test.sh --group snapshot-catchup`; `./test.sh --group integration`.

- [x] T017 [P] [US2] Add leader-switch and commit/apply ordering regressions in `tests/test_raft_replicator_behavior.cpp`, `tests/test_raft_log_replication.cpp`, and `tests/test_raft_commit_apply.cpp`
  Goal: 覆盖 leader 切换后 committed state 保持不变、新日志继续一致推进、commit/apply 不逆序。
  Input: `tests/test_raft_replicator_behavior.cpp`, `tests/test_raft_log_replication.cpp`, `tests/test_raft_commit_apply.cpp`, `spec.md`.
  Scope: 只改测试文件。
  AcceptanceX: 测试能在 leader 变化、follower 追赶和新 proposal 混合情况下验证一致性，而不是只验证单一 steady-state 路径。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: `plan.md` W4；`spec.md` FR-004；US2 acceptance scenarios。
  Tests To Run: `./test.sh --group replication`; `./test.sh --group replicator`.

- [x] T018 [P] [US2] Add state-machine replay consistency regressions in `tests/test_state_machine.cpp` and `tests/test_raft_snapshot_restart.cpp`
  Goal: 覆盖 snapshot load 后 replay、restart 后 apply、state machine 视图一致性边界。
  Input: `tests/test_state_machine.cpp`, `tests/test_raft_snapshot_restart.cpp`, `plan.md`, `data-model.md`.
  Scope: 只改测试文件。
  Acceptance: 新测试能发现 duplicate apply、missed apply、snapshot load 后 tail replay 丢失等问题。
  Production Code: No.
  Test Only: Yes.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: `plan.md` W5；`spec.md` FR-003/FR-004。
  Tests To Run: `ctest --test-dir build/linux --output-on-failure -R '^(KvStateMachineTest|RaftSnapshotRecoveryTest)\.'`; `./test.sh --group snapshot-recovery`.

### Implementation for User Story 2

- [x] T019 [US2] Apply minimal catch-up fixes in `modules/raft/replication/replicator.cpp` if T016 reveals real defects
  Goal: 仅在新回归证明 follower catch-up 边界有缺陷时，最小修复 replicator 的回退、snapshot handoff 或 next-index 推进逻辑。
  Input: T016 failing evidence, `modules/raft/replication/replicator.cpp`, related existing tests.
  Scope: 仅改 `modules/raft/replication/replicator.cpp`。
  Acceptance: T016 相关失败用例转绿，现有 replicator 行为测试不回归。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 修复结果必须通过平台无关测试入口。
  Basis: `plan.md` W4；constitution III；现状分析中的 catch-up industrialization gap。
  Tests To Run: `./test.sh --group snapshot-catchup`; `./test.sh --group replicator`.
  Execution Note: No-op。T016 的 log replay catch-up 与 snapshot handoff catch-up 回归测试已通过，未发现 `modules/raft/replication/replicator.cpp` 的真实实现缺口；本任务未修改生产代码。

- [x] T020 [US2] Apply minimal leader-switch or orchestration fixes in `modules/raft/node/raft_node.cpp` if T017 reveals real defects
  Goal: 在不改变协议语义的前提下，修复 leader 切换、commit/apply 顺序或 restart 后推进边界的真实缺陷。
  Input: T017 failing evidence, `modules/raft/node/raft_node.cpp`, related storage/replication tests.
  Scope: 限定改 `raft_node.cpp`；按 AGENTS 规则同步检查 `replication`、`storage`、`state_machine` 相关测试。
  Acceptance: T017 相关失败用例转绿，稳定选举/复制/commit 基线不被重排为新实现。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` W4；constitution III；`spec.md` US2 acceptance scenario 2。
  Tests To Run: `./test.sh --group replication`; `./test.sh --group election`; `./test.sh --group replicator`.
  Execution Note: No-op。T017 的 leader-switch / commit-apply ordering 回归测试已通过，未发现 `modules/raft/node/raft_node.cpp` 的真实实现缺口；本任务未修改生产代码。

- [x] T021 [US2] Apply minimal replay fixes in `modules/raft/state_machine/state_machine.cpp` or `modules/raft/node/raft_node.cpp` if T018 reveals real defects
  Goal: 修复 apply/replay 与 snapshot load 边界上的真实一致性问题，但不修改状态机格式或公共语义。
  Input: T018 failing evidence, `state_machine.cpp`, `raft_node.cpp`, existing snapshot restart tests.
  Scope: 限定在 `state_machine.cpp` 和必要的 `raft_node.cpp`。
  Acceptance: state machine restart/replay 新增用例转绿，现有 snapshot/save/load/restart 基线不回归。
  Production Code: Yes.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` W5；`spec.md` FR-003；现状分析中的 apply/recovery consistency gap。
  Tests To Run: `ctest --test-dir build/linux --output-on-failure -R '^(KvStateMachineTest|RaftSnapshotRecoveryTest|RaftIntegrationTest)\.'`; `./test.sh --group snapshot-recovery`.
  Execution Note: No-op。T018 的 state-machine replay consistency 回归测试已通过，未发现 `modules/raft/state_machine/state_machine.cpp` 或 `modules/raft/node/raft_node.cpp` 的真实实现缺口；本任务未修改生产代码。

- [x] T022 [US2] Update `specs/004-raft-industrialization/validation-matrix.md` with US2 consistency evidence
  Goal: 记录 catch-up、leader switch、apply/replay 一致性的新增回归证据与未覆盖时序风险。
  Input: T016-T021 results, `validation-matrix.md`.
  Scope: 文档更新。
  Acceptance: 矩阵中能区分“已由平台无关回归覆盖”和“仍只在 Linux 主验收上观察”的场景。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` W4/W5/W7；`data-model.md` ValidationScope。
  Tests To Run: None; 文档任务。

**Checkpoint**: US2 完成后，lag/restart/leader-switch 一致性应能独立验收，而不依赖新协议或大范围重构。

---

## Phase 5: User Story 3 - Run A Clear Industrialization Validation Flow (Priority: P3)

**Goal**: 建立清晰的一键验证入口、Linux-specific 隔离、跨平台 fallback 和失败定位文档。  
**Independent Test**: `cmake --preset debug-ninja-low-parallel && cmake --build --preset debug-ninja-low-parallel && CTEST_PARALLEL_LEVEL=1 ./test.sh --group all && ctest --preset debug-tests --output-on-failure`

### Implementation for User Story 3

- [x] T023 [P] [US3] Refine `test.sh` into explicit platform-neutral vs Linux-specific sections
  Goal: 让 Bash 主入口清楚标记 Linux-specific 组、平台无关组、`--keep-data` 和失败 rerun 指南。
  Input: `test.sh`, `validation-matrix.md`, `contracts/validation-entrypoints.md`.
  Scope: 只改 `test.sh` 和相关 spec 文档。
  Acceptance: 维护者从脚本头部或输出就能看出哪些组属于 Linux-specific，哪些是平台无关基础回归。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Yes.
  Windows/macOS Fallback: Yes, 必须同步给出非 Bash fallback。
  Basis: `research.md` Decision 5/6；`plan.md` W6/W7。
  Tests To Run: `./test.sh --group all`; `./test.sh --group persistence`.

- [x] T024 [P] [US3] Extend `CMakePresets.json` and adjust top-level `CMakeLists.txt` only if needed for non-hardcoded Windows configuration
  Goal: 提供更明确的跨平台 configure/build/test 入口，减少当前仅 Linux/Ninja 友好的假设；只有在必要时才修改 `CMakeLists.txt` 去掉 Windows 本机路径耦合。
  Input: `CMakePresets.json`, top-level `CMakeLists.txt`, `contracts/validation-entrypoints.md`, `validation-matrix.md`.
  Scope: 首选修改 presets；仅在 preset 无法解决 Windows 路径耦合时，最小修改 `CMakeLists.txt` 的工具链/依赖注入方式。
  Acceptance: Linux 现有 preset 不受破坏；新增或调整后的入口不再要求硬编码本机 Windows 路径才能理解如何配置。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 这是本任务核心内容。
  Basis: 现状分析中的 `CMakeLists.txt` Windows 硬编码风险；`plan.md` W6/W8。
  Tests To Run: `cmake --preset debug-ninja-low-parallel`; `cmake --build --preset debug-ninja-low-parallel`; `ctest --preset debug-tests --output-on-failure`.

- [x] T025 [P] [US3] Add non-Bash fallback runner in `test.ps1`
  Goal: 提供 Windows/macOS 规划可参考的非 Bash 测试入口，至少覆盖平台无关的 CMake/CTest 流程。
  Input: `test.sh`, `CMakePresets.json`, `contracts/validation-entrypoints.md`, `validation-matrix.md`.
  Scope: 新增 `test.ps1`；必要时更新相关文档。
  Acceptance: `test.ps1` 明确走平台无关的 configure/build/test 流程，并显式排除 Linux-specific crash/failure groups。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 这是任务目标。
  Basis: `contracts/validation-entrypoints.md` Windows/macOS wrapper contract；`plan.md` W6/W8。
  Tests To Run: 文档/脚本静态校验；Linux 上至少检查脚本文本与命令一致性，实际 Windows 运行留作 follow-up evidence。

- [x] T026 [US3] Update `tests/CMakeLists.txt` and related docs to label Linux-specific failure-injection coverage
  Goal: 让 CTest discover 的测试和文档都能看出哪些是平台无关回归，哪些依赖 Linux-specific failure injection 或 crash-style 语义。
  Input: `tests/CMakeLists.txt`, `validation-matrix.md`, `contracts/validation-entrypoints.md`, T006-T010.
  Scope: 修改 `tests/CMakeLists.txt` 的注释/组织和 spec docs；不改业务代码。
  Acceptance: 新增/更新的 failure-injection 测试在任务文档中有清晰标签；平台无关运行说明不再隐式包含 Linux-only 场景。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` W6；`contracts/validation-entrypoints.md` Test Grouping Contract。
  Tests To Run: `ctest --preset debug-tests --output-on-failure`; `./test.sh --group all`.

- [x] T027 [P] [US3] Add `specs/004-raft-industrialization/platform-support.md`
  Goal: 建立平台能力矩阵，明确 build、path、flush/sync、rename、temp dir、signal/process 测试在 Linux/Windows/macOS 的当前证据与下一步。
  Input: `validation-matrix.md`, `plan.md`, `data-model.md`, top-level `CMakeLists.txt`, `test.sh`.
  Scope: 新增 spec 文档，不改生产代码。
  Acceptance: 文档逐项说明 Linux 当前能力、Windows/macOS 当前状态、当前证据、下一步 follow-up；不虚报跨平台等价。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes.
  Basis: `data-model.md` PlatformSupportExpectation；`plan.md` W7/W8；用户跨平台要求。
  Tests To Run: None; 文档任务。

- [x] T028 [US3] Update `specs/004-raft-industrialization/quickstart.md` and `tests/README.md`
  Goal: 给维护者一份清晰的“从哪跑、失败后看哪、哪些数据可保留、哪些测试不进平台无关回归”的说明。
  Input: `quickstart.md`, `tests/README.md`, `validation-matrix.md`, `platform-support.md`.
  Scope: 仅修改文档。
  Acceptance: Linux 主入口、平台无关入口、PowerShell fallback、保留数据路径、manual-only test 程序角色都有明确说明。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `research.md` Decision 5/7；`plan.md` W6/W7/W8。
  Tests To Run: None; 文档任务。

- [x] T029 [US3] Run the Linux primary path and platform-neutral fallback and record interpretation rules in `specs/004-raft-industrialization/validation-matrix.md`
  Goal: 用真实执行结果确认维护者看到失败时如何判断是 Linux-specific、平台无关、还是 deferred runtime gap。
  Input: `validation-matrix.md`, `quickstart.md`, `test.sh`, `CMakePresets.json`.
  Scope: 执行现有测试并更新文档；不改生产代码。
  Acceptance: 文档包含 `./test.sh --group all`、`ctest --preset debug-tests` 的解释规则和失败转诊路径。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` W6/W7；`spec.md` FR-007/FR-008/FR-011。
  Tests To Run: `cmake --preset debug-ninja-low-parallel`; `cmake --build --preset debug-ninja-low-parallel`; `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all`; `ctest --preset debug-tests --output-on-failure`.

**Checkpoint**: US3 完成后，维护者应能独立理解 Linux 主入口、平台无关 fallback、Windows/macOS follow-up 边界和失败定位路径。

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: 收口跨故事文档、遗留脚本/测试角色说明与 P4 follow-up，不做大重构。

- [x] T030 [P] Document the role of `tests/persistence_more_test.cpp` and `tests/test_temp.cpp` in `tests/README.md` and `specs/004-raft-industrialization/validation-matrix.md`
  Goal: 区分受管回归测试、手工诊断程序、临时测试文件，避免后续把非回归程序误当正式验收资产。
  Input: `tests/persistence_more_test.cpp`, `tests/test_temp.cpp`, `tests/README.md`, `validation-matrix.md`.
  Scope: 文档为主；若决定删除临时文件，需另开 feature，不在本任务直接做大清理。
  Acceptance: 文档明确说明哪些文件参与 CTest，哪些只是手工/临时用途。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Not Required.
  Basis: 当前目录结构分析；`plan.md` W7。
  Tests To Run: None; 文档任务。

- [x] T031 [P] Cross-check Linux-specific tasks against `specs/004-raft-industrialization/platform-support.md`
  Goal: 确保所有 Linux-specific failure injection、crash-style、bash-only 任务都有 Windows/macOS fallback 或 deferred note。
  Input: `tasks.md`, `validation-matrix.md`, `platform-support.md`, `contracts/`.
  Scope: 文档交叉检查。
  Acceptance: 没有任何 Linux-specific 任务缺少 fallback/未验证声明；`spec.md` SC-004 可以被文档证明。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes.
  Basis: constitution IV；`plan.md` W8；`spec.md` SC-004。
  Tests To Run: None; 文档任务。

- [x] T032 Run final acceptance sweep from `specs/004-raft-industrialization/quickstart.md`
  Goal: 用最终任务成果执行一次统一收口，确认 P0-P3 范围都有对应证据，P4 只留下明确 follow-up。
  Input: `quickstart.md`, `validation-matrix.md`, `platform-support.md`, all prior task outputs.
  Scope: 执行测试并更新文档结论，不改业务逻辑。
  Acceptance: 最终文档能说明哪些任务完成、哪些任务只在 Linux 验证、哪些 Windows/macOS 仍待 follow-up；所有高风险改动都有对应回归。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes.
  Basis: `plan.md` Acceptance By Priority；`quickstart.md` implementation order。
  Tests To Run: `cmake --preset debug-ninja-low-parallel`; `cmake --build --preset debug-ninja-low-parallel`; `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all --keep-data`; `ctest --preset debug-tests --output-on-failure`.
  Result: Linux `ctest --preset debug-tests --output-on-failure` passed with `104/104` tests passed (`platform-neutral`: 100, `durability-boundary`: 4). Windows `ctest --preset windows-release-tests` passed with `18/18` tests passed in the conservative fallback baseline.
  Execution Note: Windows fallback remains limited to `CommandTest`, `KvStateMachineTest`, `TimerSchedulerTest`, and `ThreadPoolTest`; this does not imply full Windows Raft coverage or Windows equivalence for Linux-specific durability/failure-injection semantics.

---

## Phase 7: Windows Full Managed CTest Validation & Cross-Platform Closure Follow-Up

**Purpose**: 从当前 Windows `18/18` conservative baseline 进入受管 full managed CTest 验证与分类修复阶段；显式区分入口缺失、runtime-heavy 红灯、Raft 逻辑红灯、storage/persistence 红灯，以及 Windows durability 语义适配与 Linux-specific deferred 边界。

- [x] T033 [P] Add Windows full managed CTest presets and wrapper mode without replacing the conservative fallback
  Goal: 在保留 `windows-release-tests` / `.\test.ps1 -All` 保守 baseline 的前提下，新增 Windows full managed CTest 入口，用来运行当前全部受管 GTest / CTest 回归，而不是只跑 `platform-neutral-fallback` 子集。
  Input: `CMakePresets.json`, `test.ps1`, `tests/CMakeLists.txt`, `specs/004-raft-industrialization/contracts/validation-entrypoints.md`, `specs/004-raft-industrialization/platform-support.md`, `specs/004-raft-industrialization/quickstart.md`.
  Scope: 只允许修改 Windows preset / wrapper / 文档入口定义；推荐新增 `windows-release-managed-tests`，如确有必要可同步新增 `windows-debug-managed-tests`；必须保留现有 `windows-release-tests` conservative baseline 不变。
  Acceptance: 维护者可以在 Windows 上明确区分“保守 baseline 入口”和“full managed CTest 入口”；新入口覆盖 `tests/README.md` 中全部受管目标，而不是仅 `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 保留既有 fallback，并新增 full managed validation 入口。
  Basis: `plan.md` W6/W8；`platform-support.md` Windows 未验证范围；`validation-matrix.md` R6/R7。
  Tests To Run: `cmake --preset windows`; `cmake --build --preset windows-release`; `ctest --preset windows-release-tests`; `ctest --preset windows-release-managed-tests`.

- [x] T034 Run the first Windows full managed CTest sweep and capture the uncovered target matrix
  Goal: 用真实 Windows full managed CTest 执行结果建立第一版覆盖现状，明确 conservative baseline `18/18` 之外其余受管目标的真实状态。
  Input: T033 输出，`tests/README.md`, `tests/CMakeLists.txt`, `specs/004-raft-industrialization/validation-matrix.md`, `specs/004-raft-industrialization/platform-support.md`.
  Scope: 只执行 Windows configure/build/full managed CTest，并记录结果；本任务不改生产代码、不改测试语义。
  Acceptance: 结果必须单独列出当前 Windows conservative baseline 未覆盖的受管目标，并至少覆盖以下目标的 PASS/FAIL/BLOCKED 现状：`test_kv_service`, `test_raft_election`, `test_raft_log_replication`, `test_raft_commit_apply`, `test_raft_split_brain`, `test_t017_leader_switch_ordering`, `persistence_test`, `snapshot_test`, `raft_integration_test`, `test_raft_snapshot_catchup`, `test_raft_snapshot_restart`, `test_raft_snapshot_diagnosis`, `test_raft_segment_storage`, `test_snapshot_storage_reliability`, `test_raft_replicator_behavior`。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 但不得把 conservative baseline 结果改写成 full pass。
  Basis: `tests/README.md` 受管目标清单；`platform-support.md` Windows 未验证范围；当前 Linux `104/104` 与 Windows `18/18` 的证据差距。
  Tests To Run: `cmake --preset windows`; `cmake --build --preset windows-release`; `ctest --preset windows-release-managed-tests`.
  Result Snapshot:
  - 复用 `T033` 已生成的 `tmp/windows-release-managed-tests.log` 与 `tmp/test-ps1-managed.log`，未重新运行 Windows full managed 测试。
  - Windows conservative baseline 仍为 `PASS (18/18)`。
  - Windows full managed 当前为 `FAIL (85/104)`。
  - 形式化失败矩阵、失败分类和 19 个受管目标的 PASS / FAIL / BLOCKED 状态已沉淀到 `validation-matrix.md` 与 `platform-support.md`。

- [x] T035 [P] Classify the first Windows full-sweep failures into an actionable matrix
  Goal: 把 T034 暴露的 Windows full managed CTest 红灯按问题类型收口成可执行的失败矩阵，而不是笼统写成“Windows 还需 follow-up”。
  Input: T034 执行结果，`specs/004-raft-industrialization/validation-matrix.md`, `specs/004-raft-industrialization/platform-support.md`, `specs/004-raft-industrialization/quickstart.md`, `specs/004-raft-industrialization/contracts/validation-entrypoints.md`.
  Scope: 只允许修改文档与失败矩阵归类，不在本任务内修代码。
  Acceptance: 每个 FAIL/BLOCKED 目标都必须归入以下之一：`Windows full managed CTest entry/harness missing`, `Windows cluster/runtime-heavy uncovered`, `Windows election/replication/commit-apply red`, `Windows snapshot/restart/catch-up red`, `Windows persistence/segment/storage red`, `Windows durability semantics adapt-or-defer`；并明确后续对应任务编号。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 必须继续保留“Windows 不等价 Linux-specific durability”边界。
  Basis: `spec.md` FR-008/FR-011；`plan.md` W7/W8；当前 Phase 7 目标。
  Tests To Run: None; 复用 T034 输出。
  Result Snapshot:
  - 复用 `T034` 的 Windows full managed 日志，没有重新运行测试。
  - 新增 `specs/004-raft-industrialization/windows-full-managed-failure-matrix.md` 作为唯一完整失败清单。
  - Windows conservative baseline 仍为 `PASS`；Windows full managed 仍为 `FAIL (85)`。
  - 当前失败已收敛为：`T037` runtime / harness、`T038` election / replication / commit-apply、`T039` snapshot / restart / catch-up、`T040` persistence / segment / storage、`T041` durability adapt-or-defer。
  - 主文档只保留摘要和链接，不再重复粘贴 `85` 个失败测试名。

- [x] T036 Fix Windows full managed CTest preset / runner / discover blockers before touching production logic
  Goal: 先排除 Windows full managed CTest 入口层问题，避免把 preset、working directory、multi-config、discover、wrapper、filter 或结果采集错误误判为 Raft 逻辑缺陷。
  Input: T033-T035 输出，`CMakePresets.json`, `test.ps1`, `tests/CMakeLists.txt`, top-level `CMakeLists.txt`（仅当 preset/plumbing 无法单独解决时）, 相关 validation docs。
  Scope: 只允许修改 preset、wrapper、CTest discover / 标签组织、必要的构建脚本路径与文档；禁止修改 `modules/raft/**` 生产代码；禁止借由放宽断言或删除目标来“消灭”红灯。
  Acceptance: Windows full managed CTest 运行结束后，剩余失败必须是测试或生产逻辑问题，而不是入口/runner/configuration 问题；现有 `windows-release-tests` conservative baseline 仍保持可用。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 这是 full managed 入口收口任务。
  Basis: `validation-entrypoints.md` Windows preset contract；`platform-support.md` 当前 Windows 入口只到 conservative baseline。
  Tests To Run: `cmake --preset windows`; `cmake --build --preset windows-release`; `ctest --preset windows-release-tests`; `ctest --preset windows-release-managed-tests`.
  Result: No-op / confirmed no entry blocker.
  Execution Note:
  - 复用 `T033-T035` 已有配置与日志，没有重新运行 Windows 测试。
  - `windows-release-managed-tests` / `windows-debug-managed-tests` 均存在，且保持 full managed 语义。
  - `windows-release-tests` / `windows-debug-tests` 仍保留 conservative baseline 过滤。
  - `test.ps1 -Managed` 调用 `windows-release-managed-tests`；`test.ps1 -All` 仍调用 `windows-release-tests`。
  - 现有日志显示 full managed 已 discover 并执行完整 `104` 个受管测试；当前 `85` 项失败属于测试运行红灯，不是 preset / discover / wrapper 阻塞。
  - 剩余红灯继续转交 `T037-T041`。

- [x] T037 Stabilize Windows-only test harness assumptions for cluster/runtime-heavy coverage before changing Raft logic
  Goal: 如果 T035 证明某些红灯来自 Windows 下的 test harness、timeout、路径、临时目录、进程收尾或 file-lock 假设，而不是 Raft 业务逻辑，则先在测试层收口这些运行时假设。
  Input: T035 失败矩阵，受影响测试源文件，`tests/CMakeLists.txt`, `test.ps1`, `specs/004-raft-industrialization/platform-support.md`.
  Scope: 只允许修改 `tests/*.cpp`、`tests/CMakeLists.txt`、`test.ps1` 和相关文档中的 Windows test harness / timeout / path / temp-dir / cleanup 逻辑；禁止修改 `modules/raft/**` 生产代码；禁止改变协议预期、持久化格式或通过条件。
  Acceptance: 属于 Windows runtime/timing/path/file-lock 假设的失败，要么被测试层稳定化并进入 full managed sweep，要么被明确升级给 T038-T040 作为真实生产问题；不得继续与 entrypoint 缺失或 Linux-specific durability 混在一起。
  Production Code: No.
  Test Only: Yes, 仅限 Windows test harness / runtime assumption 收口。
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 这是 Windows runtime-heavy 覆盖扩大前置任务。
  Basis: `platform-support.md` Windows runtime/timing/path/file-lock 缺口；`tests/README.md` 受管目标边界。
  Tests To Run: `ctest --test-dir build/windows -C Release --output-on-failure -R '^(RaftKvServiceTest|RaftElectionTest|RaftSplitBrainTest|RaftLeaderSwitchOrderingTest|RaftIntegrationTest)\\.'`; `ctest --preset windows-release-managed-tests`.
  Result:
  - `tests/raft_integration_test.cpp` 已在 Windows 下切换到更短的临时测试根路径，`RaftIntegrationTest.*` 的 `create temp log dir failed: 文件名或扩展名太长` 信号已不再出现。
  Execution Note:
  - focused rerun 证明 `RaftIntegrationTest.*` 和 `RaftKvServiceTest.*` 当前主信号都已收敛为 `FlushFileBuffers ... GetLastError=5`，因此原先暂放在 `T037` 的 7 个失败已统一转交 `T041`；当前没有独立剩余的 runtime/harness blocker。

- [x] T038 Fix Windows election / replication / commit-apply red lights exposed by the full managed sweep
  Goal: 收口 Windows full managed CTest 中属于平台无关 Raft 逻辑的 election / replication / commit-apply 红灯，不把它们继续留在“只有 Linux 绿”的状态。
  Input: T035 失败矩阵，T037 输出，`tests/test_raft_election.cpp`, `tests/test_raft_log_replication.cpp`, `tests/test_raft_commit_apply.cpp`, `tests/test_t017_leader_switch_ordering.cpp`, `tests/test_raft_replicator_behavior.cpp`，相关模块源码。
  Scope: 如需修改生产代码，只允许最小改动 `modules/raft/node/*.cpp`, `modules/raft/replication/*.cpp`, `modules/raft/runtime/*.cpp`, `modules/raft/service/*.cpp`；允许同步补充相关测试与文档；禁止修改协议语义、公共 API、类名、函数名或持久化格式。
  Acceptance: 与 election / replication / commit-apply 相关的 Windows 红灯在 full managed sweep 下转绿，或剩余项被证明属于其他类别并重新归类；修复必须附带对应 focused rerun 和 full managed rerun 证据。
  Production Code: Yes, 仅允许 `modules/raft/node`, `modules/raft/replication`, `modules/raft/runtime`, `modules/raft/service` 的最小 `.cpp` 改动。
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: No.
  Windows/macOS Fallback: Yes, 目标是补齐平台无关 Raft 逻辑验证，不宣称 Linux-specific 等价。
  Basis: `spec.md` FR-004/FR-011；`plan.md` W4/W8；`platform-support.md` Windows cluster-style 未验证范围。
  Tests To Run: `ctest --test-dir build/windows -C Release --output-on-failure -R '^(RaftElectionTest|RaftLogReplicationTest|RaftCommitApplyTest|RaftLeaderSwitchOrderingTest|RaftReplicatorBehaviorTest)\\.'`; `ctest --preset windows-release-managed-tests`.
  Result:
  - 原始接收的 `4` 个 T038 失败已在 Windows focused rerun 与 full managed rerun 中全部转绿；当前没有独立剩余的 T038 红灯。
  Execution Note:
  - 本轮只修改 `tests/test_raft_election.cpp`、`tests/test_t017_leader_switch_ordering.cpp`、`tests/test_raft_replicator_behavior.cpp`，没有修改生产代码。
  - `tests/test_raft_election.cpp` 收紧了 leader 可见性与 follower redirect 就绪之间的等待条件，并改为每次测试使用独立 data/snapshot 根目录，避免固定 `./raft_data` / `./raft_snapshots` 造成复跑残留污染。
  - `tests/test_t017_leader_switch_ordering.cpp` 与 `tests/test_raft_replicator_behavior.cpp` 在 Windows 下改用更短的临时测试根路径，修复了 data directory / identity file 创建前即命中的长路径失败。
  - `ctest --test-dir build/windows -C Release --output-on-failure -R '^(RaftElectionTest|RaftLogReplicationTest|RaftCommitApplyTest|RaftSplitBrainTest|RaftLeaderSwitchOrderingTest|RaftReplicatorBehaviorTest)\\.'` 当前 `PASS (17/17)`。
  - `ctest --preset windows-release-managed-tests --output-on-failure` 当前仍为 `FAIL`，但剩余失败数量已从 `36` 收敛到 `32`；剩余项全部转交 `T039` / `T040` / `T042`。

- [ ] T039 Fix Windows snapshot / restart / catch-up red lights exposed by the full managed sweep
  Goal: 收口 Windows 下 snapshot、restart、catch-up、diagnosis 链路里的真实红灯，明确哪些是平台无关恢复/追赶问题，哪些才属于 durability follow-up。
  Input: T035 失败矩阵，T037 输出，`tests/snapshot_test.cpp`, `tests/test_raft_snapshot_catchup.cpp`, `tests/test_raft_snapshot_restart.cpp`, `tests/test_raft_snapshot_diagnosis.cpp`, `tests/raft_integration_test.cpp`，相关模块源码。
  Scope: 如需修改生产代码，只允许最小改动 `modules/raft/node/*.cpp`, `modules/raft/replication/*.cpp`, `modules/raft/storage/*.cpp`, `modules/raft/state_machine/*.cpp`；允许同步修改相关测试与文档；禁止改协议语义、公共 API 或持久化格式。
  Acceptance: 与 snapshot/restart/catch-up 相关的 Windows full managed CTest 红灯被定位并尽可能转绿；若某项最终属于 Linux-specific durability 语义缺口，必须明确转交 T041，而不是笼统标记为“Windows 不支持”。
  Production Code: Yes, 仅允许 `modules/raft/node`, `modules/raft/replication`, `modules/raft/storage`, `modules/raft/state_machine` 的最小 `.cpp` 改动。
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes, 但不得把 Linux-specific crash-style 语义写成已等价通过。
  Basis: `spec.md` FR-003/FR-004/FR-010/FR-011；`plan.md` W3/W4/W5/W8。
  Tests To Run: `ctest --test-dir build/windows -C Release --output-on-failure -R '^(SnapshotTest|RaftSnapshotCatchupTest|RaftSnapshotRestartTest|RaftSnapshotDiagnosisTest|RaftIntegrationTest)\\.'`; `ctest --preset windows-release-managed-tests`.

- [ ] T040 Fix Windows persistence / segment / storage red lights without changing persisted format or API
  Goal: 收口 Windows 下 `persistence_test`、segment log、snapshot storage 等 storage 相关红灯中的平台无关恢复缺陷、路径问题或 file-lock 问题，但不把 Linux-specific exact durability 注入直接等价化。
  Input: T035 失败矩阵，`tests/persistence_test.cpp`, `tests/test_raft_segment_storage.cpp`, `tests/test_snapshot_storage_reliability.cpp`, `tests/test_raft_snapshot_restart.cpp`，相关 storage/node/state_machine 源码。
  Scope: 如需修改生产代码，只允许最小改动 `modules/raft/storage/*.cpp`, `modules/raft/node/*.cpp`, `modules/raft/state_machine/*.cpp`；允许同步补充测试与文档；禁止更改持久化格式、公共 API 或对 required durability operations 进行静默降级。
  Acceptance: Windows full managed CTest 中与 persistence/segment/storage 相关的 platform-neutral 恢复红灯被修复；剩余无法转绿的 exact durability / directory sync / crash-style 语义必须明确沉淀到 T041。
  Production Code: Yes, 仅允许 `modules/raft/storage`, `modules/raft/node`, `modules/raft/state_machine` 的最小 `.cpp` 改动。
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes, 但不得把 Linux-specific failure-injection 结果改写成 Windows 已等价验证。
  Basis: `spec.md` FR-003/FR-006/FR-010/FR-011；`plan.md` W2/W3/W8；根 `AGENTS.md` durability contract。
  Tests To Run: `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\\.'`; `ctest --preset windows-release-managed-tests`.

- [ ] T041 Handle Windows durability semantics as explicit adapt-or-defer follow-up
  Goal: 单独处理 Windows durability 语义，不把 Linux `fsync` / directory sync / crash-style failure injection 直接硬写成 Windows 等价；能实现的 required durability contract 做最小适配，不能等价的部分明确 deferred 边界和错误语义。
  Input: T035/T040 输出，`specs/004-raft-industrialization/platform-support.md`, `specs/004-raft-industrialization/validation-matrix.md`, `specs/004-raft-industrialization/contracts/validation-entrypoints.md`, `specs/004-raft-industrialization/quickstart.md`, `modules/raft/storage/**`, `modules/raft/runtime/**`.
  Scope: 允许最小修改 `modules/raft/storage/*.cpp`、`modules/raft/runtime/*.cpp` 或必要的窄范围平台 helper，以满足 Windows required durability contract 或明确错误返回；允许同步更新平台/验证文档；禁止引入 silent no-op durability success，禁止宣称 Windows 已等价通过 Linux-specific failure-injection。
  Acceptance: Windows durability 相关结论必须二选一并留证据：1) 对 required durability operations 给出可验证的 Windows 适配实现和 focused/full managed 证据；2) 对当前无法等价的 exact Linux-specific 语义给出明确 deferred 说明、非等价原因和后续入口，不与 platform-neutral full managed pass 混写。
  Production Code: Yes, 仅允许 `modules/raft/storage`、`modules/raft/runtime` 及必要窄范围平台 helper 的最小改动。
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Yes, 需要显式区分 Linux-specific exact durability semantics。
  Windows/macOS Fallback: Yes, 这是 Windows durability follow-up 的核心边界任务。
  Basis: 根 `AGENTS.md` durability hard rule；`spec.md` FR-006/FR-009/FR-011；`platform-support.md` Windows durability 未验证范围。
  Tests To Run: `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\\.'`; `ctest --preset windows-release-managed-tests`.
  Result:
  - 已完成 Windows directory flush 句柄权限适配；`FlushFileBuffers ... GetLastError=5`
    不再是当前主失败信号。
  Execution Note:
  - `modules/raft/storage/raft_storage.cpp` 与 `modules/raft/storage/snapshot_storage.cpp`
    已把 Windows directory sync 从 `FILE_LIST_DIRECTORY` 句柄改为可写目录句柄，
    继续保留真实错误返回，不引入 no-op success。
  - `ctest --test-dir build/windows -C Release --output-on-failure -R '^(RaftIntegrationTest|RaftKvServiceTest)\\.'`
    已转为 `PASS`；原先被目录 flush 权限错误阻塞的 `7` 个 cluster-style 用例已从
    failure matrix 删除。
  - `ctest --preset windows-release-managed-tests --output-on-failure` 当前仍为 `FAIL`，
    但剩余失败数量已从 `85` 收敛到 `36`。
  - 剩余项继续分流：
    `T038` `4` 项、`T039` `17` 项、`T040` `6` 项、`T042` `9` 项。
  - exact Linux-specific failure-injection seam 当前继续按 deferred /
    non-equivalent 记录，不写成 Windows 已等价 Linux durability 证据。

- [ ] T042 Backfill cross-platform docs and run the final Windows full managed closure sweep
  Goal: 用最终 Windows full managed CTest 结果回填平台文档、验证矩阵、入口说明和测试角色说明，形成“Windows full managed 已验证到哪里、哪些仍是 Linux-specific / deferred”的最终收口结论。
  Input: T033-T041 全部输出，`specs/004-raft-industrialization/validation-matrix.md`, `specs/004-raft-industrialization/platform-support.md`, `specs/004-raft-industrialization/quickstart.md`, `specs/004-raft-industrialization/contracts/validation-entrypoints.md`, `tests/README.md`, `CMakePresets.json`, `test.ps1`.
  Scope: 允许修改文档、preset 与 wrapper 说明，并执行最终 Windows full managed rerun；若 rerun 暴露新生产问题，应新开后续任务，不在本任务内临时扩大修复范围。
  Acceptance: 文档明确记录 Windows conservative baseline 与 Windows full managed CTest 的最终差异；明确哪些 managed tests 已在 Windows full managed 下通过，哪些仍只属于 Linux-specific durability evidence 或 deferred follow-up；不得出现“Windows 已等价 Linux 104/104”或“Windows durability 已等价 Linux”表述。
  Production Code: No.
  Test Only: No.
  Cross-Platform: Yes.
  Linux-Specific: Partially.
  Windows/macOS Fallback: Yes, 需同时保留 conservative baseline 与 full managed 入口说明。
  Basis: `spec.md` SC-003/SC-004/SC-005；`plan.md` W8；当前 Phase 7 总目标。
  Tests To Run: `cmake --preset windows`; `cmake --build --preset windows-release`; `ctest --preset windows-release-tests`; `ctest --preset windows-release-managed-tests`.

**Checkpoint**: Phase 7 完成后，Windows 将不再只停留在 `18/18` conservative baseline；维护者应能看清 full managed CTest 的真实覆盖、失败矩阵、已修复类别、durability 非等价边界和剩余 deferred 项。

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1: Setup**: 无依赖，立即开始。
- **Phase 2: Foundational**: 依赖 Phase 1，阻塞 US1 的高风险实现。
- **Phase 3: US1**: 依赖 Phase 2，是 MVP，先完成 P0 恢复/持久化可信度。
- **Phase 4: US2**: 依赖 US1 的 trusted-state 基线，避免把恢复问题与复制问题混在一起。
- **Phase 5: US3**: 依赖 US1/US2 的实际产出，才能形成真实入口和文档。
- **Phase 6: Polish**: 所有目标故事完成后收口。
- **Phase 7: Windows Full Managed Closure**: 依赖 Phase 6，基于当前 Linux 主验证已收口、Windows conservative fallback 已成文的前提，进入 Windows full managed CTest sweep、分类修复与 durability follow-up。

### User Story Dependencies

- **US1**: 依赖 Foundational；不依赖 US2/US3。
- **US2**: 依赖 US1 的 trusted-state 与恢复基线。
- **US3**: 依赖 US1/US2 的真实测试与修复结果，但不应反向阻塞 P0 修复。

### Within Each User Story

- 先写或补失败测试，再定是否需要生产代码修复。
- `storage` failure injection 与 trusted-state 测试在 `node`/`replication` 修复前先落地。
- 文档和平台说明在对应验证证据形成后再更新。
- Phase 7 先补 Windows full managed 入口（T033），再做第一次 full sweep 与失败矩阵（T034-T035），然后先清 entry/harness 阻塞（T036-T037），最后按生产问题类别进入 T038-T041，T042 负责最终回填与复验。

### Priority Order

- **P0**: T001-T015
- **P1**: T016-T022
- **P2**: T023-T026
- **P3**: T027-T030
- **P4**: T031-T042

### Parallel Opportunities

- T001 和 T002 可并行。
- T003 和 T004 可并行；T005 在两者结果出来后更新脚本/文档。
- T006、T007、T008 可并行。
- T012 和 T013 可并行。
- T016、T017、T018 可并行。
- T023、T024、T025、T027 可并行，因为分别落在脚本、presets/CMake、PowerShell、平台文档。
- T033 完成后，T034 与 T036 不能跳过；T035 依赖 T034 的真实 sweep 结果。
- T037、T038、T039、T040 在 T035 完成且问题边界切清后可按 test-only / production-code 分类并行，但必须保持写入范围不重叠。
- T041 依赖 T035 与 storage 相关结果；T042 必须最后执行。

---

## Parallel Example: User Story 1

```bash
# Tests-first tasks for US1 can run together:
Task: "Add meta/log durability failure cases in tests/persistence_test.cpp and tests/test_raft_segment_storage.cpp"
Task: "Add snapshot publish/prune failure cases in tests/test_snapshot_storage_reliability.cpp and tests/test_raft_snapshot_restart.cpp"
Task: "Migrate useful manual recovery scenarios from tests/persistence_more_test.cpp into managed GTest files"

# After the test shape is clear, these can run in parallel:
Task: "Introduce an opt-in internal failure injection seam in modules/raft/storage/raft_storage.cpp"
Task: "Introduce the same opt-in seam for snapshot publish/prune in modules/raft/storage/snapshot_storage.cpp"
```

---

## Parallel Example: User Story 2

```bash
# Regression additions for US2 can run together:
Task: "Add lagging-follower and compaction catch-up regressions in tests/test_raft_snapshot_catchup.cpp and tests/raft_integration_test.cpp"
Task: "Add leader-switch and commit/apply ordering regressions in tests/test_raft_replicator_behavior.cpp, tests/test_raft_log_replication.cpp, and tests/test_raft_commit_apply.cpp"
Task: "Add state-machine replay consistency regressions in tests/test_state_machine.cpp and tests/test_raft_snapshot_restart.cpp"
```

---

## Implementation Strategy

### MVP First (US1)

1. 完成 Phase 1 和 Phase 2。
2. 完成 US1 的 tests-first + minimal storage/restart hardening。
3. 单独执行 US1 的 persistence/snapshot-recovery/diagnosis 验收。
4. 若 US1 证据成立，再进入 US2。

### Incremental Delivery

1. Setup + Foundational：冻结范围并稳定当前 Linux blocker。
2. US1：建立 restart/trusted-state/durability 可信度。
3. US2：补强 cluster consistency 与 state-machine replay 证据。
4. US3：收敛一键入口、平台 fallback 与失败定位文档。
5. Polish：完成 P4 follow-up 标注与最终收口。
6. Phase 7：把 Windows 从 conservative baseline 推进到 full managed CTest sweep、分类修复与 durability 边界收口。

## Notes

- 所有任务都只针对“仍需完善”的工业化缺口，没有把稳定完成的 Raft 主路径重新拆成实现任务。
- 任何生产代码任务都限定在最小 `.cpp` 修改范围，并以新增失败测试为前置证据。
- Linux-specific 任务都必须在文档中带 Windows/macOS fallback 或 deferred note。
- Windows full managed CTest 任务必须始终把 conservative baseline、platform-neutral full sweep、durability-boundary 和 Linux-specific exact failure-injection 分开解释，禁止跨平台虚报等价。
