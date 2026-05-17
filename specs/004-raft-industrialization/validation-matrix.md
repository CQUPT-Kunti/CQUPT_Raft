# CQUPT_Raft Industrialization Validation Matrix

## Purpose

This document freezes the industrialization scope for
`004-raft-industrialization` so completed work is carried forward as protected
baseline, while only the remaining reliability, recovery, validation, and
cross-platform gaps are scheduled for follow-up work.

## Sources

- `specs/004-raft-industrialization/spec.md`
- `specs/004-raft-industrialization/plan.md`
- `specs/004-raft-industrialization/quickstart.md`
- `specs/004-raft-industrialization/platform-support.md`
- `specs/003-persistence-reliability/progress.md`
- `test.sh`

## Scope Freeze

### Protected Baseline (Already Completed, Do Not Re-Implement)

| Area | Current status | Carry-forward evidence | Validation status | Next action |
|------|----------------|------------------------|-------------------|-------------|
| Leader election, AppendEntries, majority commit, apply progression | Implemented and already test-backed | Current `spec.md` baseline, existing election/replication/apply tests | Baseline preserved | Regression only |
| Snapshot save/load/install baseline | Implemented and already test-backed | `spec.md` baseline, snapshot/restart/catch-up tests | Baseline preserved | Regression only |
| Follower catch-up baseline | Implemented, but still needs stronger industrialization evidence | Existing catch-up and replicator tests | Baseline preserved, not reimplemented | Add targeted regression only |
| Segmented log persistence baseline | Implemented and strengthened in `003` | `003 progress.md` T201-T206 | Completed | Reuse as existing capability |
| Hard-state durability for `meta.bin` | Implemented and strengthened in `003` | `003 progress.md` T301-T318 | Completed | Reuse as existing capability |
| Snapshot atomic publish baseline | Implemented and strengthened in `003` | `003 progress.md` T401-T423 | Completed | Reuse as existing capability |
| Restart recovery diagnostics baseline | Implemented and strengthened in `003` | `003 progress.md` T501-T524 | Completed | Reuse as existing capability |
| Crash-artifact recovery coverage from malformed files/directories | Implemented in `003` without production rewrite | `003 progress.md` T601-T622 | Completed for constructed artifacts | Extend only where exact injection is still missing |

### Remaining Industrialization Work (In Scope)

| Priority | Area | Current status | Risk source | Primary validation | Linux-specific | Windows/macOS fallback |
|----------|------|----------------|-------------|--------------------|----------------|------------------------|
| P0 | Final Linux validation flaky stabilization | Not complete | `003 progress.md` blocked items for snapshot recovery and split-brain timing failures | `./test.sh --group snapshot-recovery`, `./test.sh --group election`, targeted `ctest -R` reruns | Yes | Platform-neutral rerun through `ctest --preset debug-tests`; no equivalent timing guarantee claimed |
| P0 | Exact durability failure injection (`fsync`, directory sync, rename/replace, prune/remove, partial write) | Missing | `003 progress.md` blocked T613-T616 | New targeted persistence/snapshot tests plus Linux reruns | Yes | Explicit deferred runtime validation; preserve documentation-only fallback |
| P0 | Restart trusted-state matrix for term/vote/log/snapshot metadata/applied state | US1 accepted; managed regression evidence in place | `spec.md` FR-003/FR-010, `plan.md` W3 | `./test.sh --group persistence`, `snapshot-recovery`, `diagnosis`; targeted `ctest -R '^(PersistenceTest|RaftSegmentStorageTest|RaftSnapshotRecoveryTest|RaftSnapshotDiagnosisTest)\.'` | Partially | Platform-neutral recovery tests remain valid; Linux-specific durability evidence is recorded separately and crash semantics are not over-claimed |
| P1 | Catch-up after lag, compaction, restart, and snapshot handoff | Implemented but needs stronger proof | `plan.md` W4, current gap classification `B/C` | `./test.sh --group snapshot-catchup`, `integration`, `replicator` | No | Same tests should remain runnable via CTest |
| P1 | Leader switch and commit/apply ordering under churn | Implemented but still risky | `plan.md` W4, current gap classification `B/C` | `./test.sh --group replication`, `election`, `replicator` | No | Same tests should remain runnable via CTest |
| P1 | State-machine apply/replay consistency after snapshot/restart | Implemented but needs stronger proof | `plan.md` W5 | `./test.sh --group snapshot-recovery`, targeted `ctest -R` on state machine and integration tests | No | Same tests should remain runnable via CTest |
| P2 | Unified validation entrypoints and grouped test guidance | Completed for current documented scope | Current `test.sh`, `test.ps1`, `CMakePresets.json`, `plan.md` W6 | `./test.sh --group all`, `.\test.ps1 -All`, `.\test.ps1 -Managed`, `ctest --preset debug-tests`, `ctest --preset windows-release-tests`, `ctest --preset windows-debug-tests`, `ctest --preset windows-release-managed-tests` | Partially | PowerShell fallback is now available; Windows conservative baseline 与 Windows full managed 入口已分开记录，Linux-specific runtime evidence 仍需显式边界说明 |
| P3 | Failure localization and platform-support documentation | Completed for current US3 scope | `plan.md` W7/W8 | Spec docs and grouped rerun commands | Partially | Platform support, quickstart, tests README, and final US3 interpretation rules are now documented; remaining items stay in explicit follow-up |
| P4 | Windows/macOS deeper runtime validation and CI expansion | Deferred follow-up | `spec.md` platform scope, `plan.md` W8 | Future runtime validation | No | This row is itself the fallback and follow-up definition |

### Explicitly Deferred (Not In Current Implementation Scope)

| Area | Why deferred |
|------|--------------|
| Protocol semantic changes | Forbidden by constitution and current task scope |
| Persisted format changes or migrations | Forbidden by constitution and current task scope |
| Public API redesign | Not required for current industrialization goals |
| Transport rewrite or snapshot streaming RPC redesign | Out of scope for this feature |
| Business storage engine expansion beyond metadata layer | Unrelated to current industrialization scope |
| Large-scale architectural refactor | Violates minimal-change constraint |

## Validation Entry Matrix

| Entrypoint | Purpose | Scope | Linux-specific | Artifact retention | Notes |
|-----------|---------|-------|----------------|--------------------|-------|
| `cmake --preset debug-ninja-low-parallel` | Primary low-parallel configure | Linux primary build path | No | No | Current preferred configure entry |
| `cmake --build --preset debug-ninja-low-parallel` | Primary low-parallel build | Linux primary build path | No | No | Current preferred build entry |
| `cmake --preset windows` | Windows configure fallback | Windows platform-neutral baseline setup | No | No | Existing Visual Studio 17 2022 configure preset remains unchanged |
| `cmake --build --preset windows-release` | Windows Release build fallback | Windows platform-neutral baseline build | No | No | Uses `configurePreset: windows`, `configuration: Release` |
| `ctest --preset windows-release-tests` | Windows Release test fallback | Windows platform-neutral baseline execution | No | No | Uses `configuration: Release`; current subset is `CommandTest|KvStateMachineTest|TimerSchedulerTest|ThreadPoolTest` |
| `ctest --preset windows-release-managed-tests` | Windows Release full managed sweep | Windows full managed CTest target sweep | No | No | Uses `configuration: Release`; runs the complete managed CTest target set; latest Windows rerun is `104/104 PASS` |
| `cmake --build --preset windows-debug` | Windows Debug build fallback | Optional Windows platform-neutral debug build | No | No | Uses `configurePreset: windows`, `configuration: Debug` |
| `ctest --preset windows-debug-tests` | Windows Debug test fallback | Optional Windows platform-neutral debug execution | No | No | Uses `configuration: Debug`; current subset is `CommandTest|KvStateMachineTest|TimerSchedulerTest|ThreadPoolTest` |
| `ctest --preset windows-debug-managed-tests` | Windows Debug full managed sweep | Optional Windows full managed CTest target sweep | No | No | Uses `configuration: Debug`; runs the complete managed CTest target set; entry exists but does not imply PASS |
| `.\test.ps1 -All` | Windows PowerShell fallback wrapper | Windows platform-neutral one-command validation | No | No | Default flow wraps `windows` / `windows-release` / `windows-release-tests`; current test subset is conservative and does not run Linux-specific groups |
| `.\test.ps1 -Managed` | Windows PowerShell full managed wrapper | Windows one-command full managed CTest sweep | No | No | Explicit opt-in mode; wraps `windows` / `windows-release` / `windows-release-managed-tests`; entry existence does not imply PASS |
| `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all` | Primary regression sweep | Linux primary validation | Partially | Optional via `--keep-data` | Includes grouped Linux execution flow |
| `./test.sh --group persistence` | Focused restart/durability rerun | Industrialization hotspot | Partially | Optional via `--keep-data` | Main trusted-state regression bucket |
| `./test.sh --group snapshot-recovery` | Focused snapshot/restart rerun | Industrialization hotspot | Partially | Optional via `--keep-data` | Current flaky blocker area |
| `./test.sh --group diagnosis` | Focused diagnostics rerun | Industrialization hotspot | Partially | Optional via `--keep-data` | Main failure-localization bucket |
| `./test.sh --group snapshot-catchup` | Focused catch-up rerun | US2 regression | No | Optional via `--keep-data` | Cluster consistency evidence |
| `./test.sh --group replicator` | Focused replicator rerun | US2 regression | No | Optional via `--keep-data` | Follower synchronization evidence |
| `./test.sh --group segment-cluster` | Focused clustered segment/snapshot stress rerun | US2/US3 regression | Partially | Optional via `--keep-data` | Main segment rollover and retained-artifact stress bucket |
| `ctest --preset debug-tests --output-on-failure` | Platform-neutral fallback | Cross-platform baseline execution contract | No | No | Must remain valid outside Bash-first flows; Linux fallback remains available alongside Windows preset path |

### T026 Linux 侧确认

当前 `T026` 范围内，已在 Linux 环境完成以下确认：

- `cmake --preset debug-ninja-low-parallel`: PASS
- `cmake --build --preset debug-ninja-low-parallel`: PASS
- `ctest --preset debug-tests --output-on-failure`: PASS
  - Linux 受管 CTest 当前 `104/104` 通过
  - Label 统计：`platform-neutral` 100 个测试，`durability-boundary` 4 个测试
  - 该结果说明 Linux CTest fallback 当前已全绿，但不改变
    Linux-specific failure-injection / durability-boundary 的平台解释边界
- `CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence`: PASS

本次验证固定的解释边界：

- Linux 主入口仍是 `./test.sh`
- `debug-tests` 仍是 Linux / cross-platform 的 CTest fallback，不替代
  Linux-primary 主验收解释
- Windows fallback 仍是保守 baseline，不记录为 Linux-specific durability /
  failure-injection 的等价验收证据

### US3 最终验证入口状态

当前 `US3` 的验证入口文档与状态收口如下：

- `T023`：已完成，Linux `./test.sh` 主入口的 platform-neutral /
  Linux-specific 分组、`--keep-data` 与 rerun 指南已明确
- `T024`：已完成，Windows `windows` / `windows-release` /
  `windows-release-tests` preset fallback 已形成稳定文档入口
- `T025`：已完成，Windows PowerShell fallback `.\test.ps1 -All` 已记录
- `T026`：已完成，`platform-neutral`、`durability-boundary`、
  `linux-specific-failure-injection`、`linux-primary-diagnosis` 标签边界已明确
- `T027`：已完成，`platform-support.md` 已记录 Linux / Windows 当前支持范围
- `T028`：已完成，`quickstart.md` 与 `tests/README.md` 已补齐维护者入口说明
- `T029`：已完成，当前 Linux / Windows 入口结果、平台边界与后续 follow-up
  已统一记录

当前文档化结果：

- Linux：
  - `cmake --preset debug-ninja-low-parallel`：PASS
  - `cmake --build --preset debug-ninja-low-parallel`：PASS
  - `CTEST_PARALLEL_LEVEL=1 ./test.sh --group persistence`：PASS
  - `ctest --preset debug-tests --output-on-failure`：PASS
  - Linux 受管 CTest 当前 `104/104` 通过
  - Label 统计：`platform-neutral` 100 个测试，`durability-boundary` 4 个测试
  - 当前不再记录 `debug-tests` / cluster-runtime 红灯
- Windows：
  - `cmake --preset windows`：PASS
  - `cmake --build --preset windows-release`：PASS
  - `ctest --preset windows-release-tests --output-on-failure`：PASS
  - `.\test.ps1 -All`：作为 Windows PowerShell fallback 已文档化
  - Windows fallback 当前只代表保守 platform-neutral baseline
  - 默认覆盖：
    `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、
    `ThreadPoolTest`

固定解释边界：

- Windows fallback 不是 Raft 全功能验收结果
- Windows fallback 不等价于 Linux-specific durability /
  failure-injection 验收
- Linux-specific durability / failure-injection / crash-style 语义仍以 Linux
  主验收解释为准
- 当前 `US3` 可以按“验证入口与状态文档已收口、运行时扩大验证仍留作
  follow-up”来理解

### T032 最终验收扫尾结果

本次 `T032` 依据最新 Linux / Windows 验证结果更新最终验收状态，只记录
命令结果与解释边界，不改变生产代码、测试源码或平台语义。

- `cmake --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --preset debug-tests --output-on-failure`
  - PASS
  - `104/104` tests passed
  - Label 统计：`platform-neutral` 100 个测试，`durability-boundary` 4 个测试
- `cmake --preset windows`
  - PASS
- `cmake --build --preset windows-release`
  - PASS
- `ctest --preset windows-release-tests --output-on-failure`
  - PASS
  - `18/18` tests passed
  - 当前只覆盖 Windows 保守 fallback baseline：`CommandTest`、
    `KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`

当前收口解释：

- P0：
  - Linux 受管 CTest 当前已全绿，`debug-tests` 不再作为现存红灯记录。
  - exact durability / failure-injection 仍继续按 Linux-specific 主验收解释，
    不转写为 Windows 等价证据。
- P1：
  - catch-up、leader switch、apply/replay 的 managed regression evidence
    继续保留为文档化证据；当前 Linux CTest 全绿进一步确认这些受管回归
    没有遗留红灯。
- P2：
  - Linux Bash 主入口、CTest fallback、Windows preset fallback、
    PowerShell fallback 的入口约定仍然成立。
  - Windows fallback 当前仍是保守 baseline，不解释为 Windows Raft 全功能通过。
- P3：
  - failure-localization、平台支持矩阵、quickstart 与 tests README 的文档
    范围已收口；Linux 当前受管 CTest 全绿，Windows fallback 保持保守 baseline。
- P4：
  - Windows 更深的 runtime validation、durability 等价语义与 CI
    扩展继续保留为 follow-up，不写成已完成。

### T034 Windows Full Managed First Sweep

本次 `T034` 直接复用 `T033` 已产生的 Windows full managed 日志：

- `tmp/windows-release-managed-tests.log`
- `tmp/test-ps1-managed.log`

本次没有重新运行 Windows full managed 测试；矩阵结论全部来自上述日志。

当前记录的 Windows 状态分为两层：

- conservative baseline：
  - `ctest --preset windows-release-tests`
  - 保持 `PASS`
  - 当前仍是 `18/18` tests passed
- full managed sweep：
  - `ctest --preset windows-release-managed-tests`
  - `.\test.ps1 -Managed`
  - `T034` 首次 sweep 结果：`104` 个受管测试里 `19` 个通过、`85` 个失败

### T035 Windows Full Managed Actionable Matrix

本次 `T035` 在不重跑测试的前提下，把 T034 的首次 Windows full managed
结果整理成单独失败矩阵：

- 完整失败测试名只保留在
  [windows-full-managed-failure-matrix.md](./windows-full-managed-failure-matrix.md)
- `validation-matrix.md` 只保留摘要、状态和链接，不再重复粘贴 `85` 个失败名
- 已通过的 Windows conservative baseline 子集
  `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`
  已从失败矩阵里删除
- Linux 结果只保留摘要：
  - Linux full managed CTest 当前 `104/104` PASS
  - `platform-neutral` 100 tests PASS
  - `durability-boundary` 4 tests PASS

当前 T035 摘要：

| 项目 | 当前状态 | 说明 |
|------|----------|------|
| Windows conservative baseline | PASS | `windows-release-tests`，当前 `18/18` 通过 |
| Windows full managed | PASS | `windows-release-managed-tests` 最近一次正式 rerun 已 `104/104` 通过；Windows full managed 当前无剩余红灯 |
| 失败详情 | 单独维护 | 详见 [windows-full-managed-failure-matrix.md](./windows-full-managed-failure-matrix.md) |
| 当前主要后续任务 | 文档收口 | 进入 `T042` 做跨平台文档回填与最终任务关闭；不再有待修失败项 |

### T036 Windows Full Managed Entrypoint Check

本次 `T036` 复用 `T033-T035` 已有配置与日志，没有重新运行 Windows 测试。

当前确认结果：

- `windows-release-managed-tests` preset 存在，且保持 full managed 语义
- `windows-release-managed-tests` / `windows-debug-managed-tests` 都未使用
  baseline 子集过滤
- `windows-release-tests` / `windows-debug-tests` 仍保留 conservative baseline
  过滤
- `test.ps1 -Managed` 确实调用 `windows-release-managed-tests`
- `test.ps1 -All` 仍调用 conservative baseline `windows-release-tests`
- Visual Studio multi-config 路径继续通过 `configuration: Release` /
  `configuration: Debug` 选配置
- full managed preset 当前 `execution.jobs` 为 `1`，`outputOnFailure` 已开启
- `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST ...)` 与当前 label 说明
  没有阻塞 full managed 测试发现
- 现有日志显示 full managed 已 discover 并执行完整 `104` 个受管测试；
  当前 `85` 项失败属于测试运行红灯，而不是 discover / preset / wrapper
  阻塞

### T037 Windows Runtime / Harness Triage

本次 `T037` 只处理 Windows 下 cluster/runtime-heavy 覆盖里的 harness 假设，
不修改生产代码，也不把真实 durability 红灯伪装成 timeout/cleanup 修复。

本次最小变更与 focused rerun 结果：

- `tests/raft_integration_test.cpp` 已在 Windows 下改用更短的临时测试根路径，
  以消除 `create temp log dir failed: 文件名或扩展名太长` 这一类长路径 blocker。
- `ctest --test-dir build/windows -C Release --output-on-failure -R '^RaftIntegrationTest\.'`
  仍为 `FAIL`，但当前 5 个失败已不再表现为长路径建目录失败，而是统一暴露为
  `FlushFileBuffers ... GetLastError=5`。
- `ctest --test-dir build/windows -C Release --output-on-failure -R '^RaftKvServiceTest\.'`
  仍为 `FAIL`；其中 2 个失败用例与上面呈现相同的
  `FlushFileBuffers ... GetLastError=5` 主信号，`DataDirectoryIdentityMismatchIsRejected`
  仍保持 `PASS`。

当前 T037 结论：

- Windows 长路径 harness 假设已对 `RaftIntegrationTest.*` 做到最小收紧。
- 原先暂放在 `T037` 的 7 个失败，当前已没有独立的 runtime/harness blocker。
- 这 7 个失败统一转交 `T041`，按 Windows durability semantics
  adapt-or-defer 继续处理。

T036 当前结论：

- `confirmed no entry blocker / no-op`
- 剩余红灯继续转交 `T037-T041`

### T038 Windows Election / Replication / Commit-Apply Closure

本次 `T038` 只处理 Windows full managed 中的 `4` 个 election / replication /
commit-apply 红灯，不进入 snapshot / persistence / durability deferred 范围。

本次最小测试层修正：

- `tests/test_raft_election.cpp`
  - 改为每次测试使用独立 data/snapshot 根目录，避免固定 `./raft_data` /
    `./raft_snapshots` 造成的复跑残留污染。
  - 在断言 follower redirect 之前，等待 leader 可见性真正传播到 follower，
    收紧“leader 已可见”与“follower 已能返回 leader_id”之间的时序空洞。
- `tests/test_t017_leader_switch_ordering.cpp`
  - Windows 下切换到更短的临时测试根路径，避免长路径在建目录阶段直接失败。
- `tests/test_raft_replicator_behavior.cpp`
  - Windows 下切换到更短的临时测试根路径，避免长路径在 identity file 创建前就失败。

本次 focused / full managed 结果摘要：

- `ctest --test-dir build/windows -C Release --output-on-failure -R '^(RaftElectionTest|RaftLogReplicationTest|RaftCommitApplyTest|RaftSplitBrainTest|RaftLeaderSwitchOrderingTest|RaftReplicatorBehaviorTest)\.'`
  - PASS
  - 当前 `17/17` 通过
- `ctest --preset windows-release-managed-tests --output-on-failure`
  - FAIL
  - 当前 full managed 剩余失败数量已从 `36` 收敛到 `32`

当前 T038 结论：

- 原始接收的 `4` 个失败已全部转绿，并已从 failure matrix 移除。
- 当前没有证据表明 `modules/raft/node`、`modules/raft/replication`、
  `modules/raft/runtime`、`modules/raft/service` 存在需要在 T038 范围内修复的
  真实跨平台生产缺陷。
- 剩余 full managed 红灯全部转交 `T039` / `T040` / `T042`。

### T039 Windows Snapshot / Restart / Catch-Up Closure

本次 `T039` 只处理 Windows full managed 中的 `17` 个 snapshot / restart /
catch-up 红灯，不进入 persistence / segment / exact seam deferred 范围。

本次最小测试层修正：

- `tests/test_raft_snapshot_catchup.cpp`
  - Windows 下改用更短的临时测试根路径。
  - 端口分配改为按测试名映射到固定间隔端口窗，避免相邻 case 在 Windows 上随机撞到刚释放的端口范围。
- `tests/test_raft_snapshot_restart.cpp`
  - Windows 下改用更短的临时测试根路径。
  - 端口分配改为按测试名映射到固定间隔端口窗，避免 snapshot/restart case 间的端口复用噪声。
- `tests/test_raft_snapshot_diagnosis.cpp`
  - Windows 下改用更短的临时测试根路径。
  - 端口分配改为按测试名映射到固定间隔端口窗，减少 diagnosis 集群 case 的随机端口冲突。

本次 focused / full managed 结果摘要：

- `ctest --test-dir build/windows -C Release --output-on-failure -R '^(SnapshotTest|RaftSnapshotCatchupTest|RaftSnapshotRestartTest|RaftSnapshotRecoveryTest|RaftSnapshotDiagnosisTest|RaftIntegrationTest)\.'`
  - FAIL
  - 当前 `24` 个 focused 用例里 `23` 个通过；唯一剩余失败为
    `RaftSnapshotRecoveryTest.RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`
- `ctest --preset windows-release-managed-tests --output-on-failure`
  - FAIL
  - 当前 full managed 剩余失败数量已从 `32` 收敛到 `15`

当前 T039 结论：

- 原始接收的 snapshot / restart / catch-up 本体红灯已全部转绿，并已从 failure matrix 删除。
- `RaftSnapshotRecoveryTest.RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`
  经复核属于 exact failure-injection seam，不再保留在 T039，已转交 `T042`。
- 当前 Windows full managed 已无独立 T039 失败；剩余红灯只转交 `T040` / `T042`。

### T041 Windows Durability Adapt-Or-Defer

本次 `T041` 聚焦 Windows durability 语义，不修改 Raft election /
replication / snapshot 业务逻辑，只处理 storage 层的 Windows flush / sync
契约与对应证据。

本次最小生产代码修复：

- `modules/raft/storage/raft_storage.cpp`
- `modules/raft/storage/snapshot_storage.cpp`

当前确认的根因：

- Windows 目录 sync 之前使用 `FILE_LIST_DIRECTORY` 打开目录句柄，随后直接对该
  目录句柄调用 `FlushFileBuffers`。
- 在当前 Windows 环境下，这会稳定触发
  `FlushFileBuffers ... GetLastError=5 (ERROR_ACCESS_DENIED)`。
- `T041` 已把 directory sync 改为使用可写目录句柄执行 flush，因此当前 focused
  rerun 已不再暴露这个主信号。

本次 focused / full managed 结果摘要：

- `ctest --test-dir build/windows -C Release --output-on-failure -R '^(RaftIntegrationTest|RaftKvServiceTest)\.'`
  - PASS
  - 原先被 `FlushFileBuffers GetLastError=5` 阻塞的 `7` 个 cluster-style 用例已转绿
- `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'`
  - FAIL
  - 当前剩余 `8` 个 focused storage / durability 失败
- `ctest --preset windows-release-managed-tests --output-on-failure`
  - FAIL
  - 在 `T039` 完成后，当前 full managed 剩余失败数量进一步收敛到 `15`

当前 T041 结论：

- `FlushFileBuffers ... GetLastError=5` 已从当前 focused / full managed 主失败信号中消失。
- Windows full managed 当前剩余 `15` 项失败，后续分流为：
  - `T040`：`6` 项 platform-neutral persistence / segment / storage 红灯
  - `T042`：`9` 项 exact Linux-specific failure-injection seam，按 deferred /
    non-equivalent 最终收口
- Windows durability 当前不能写成“已等价 Linux-specific failure-injection”；
  只能写成“required directory flush contract 已做 Windows 适配，exact seam
  仍显式 deferred / non-equivalent”。

### T040 Windows Persistence / Segment / Storage Closure

本次 `T040` 只处理 Windows full managed 中 `6` 个 platform-neutral persistence /
segment / storage 红灯，不进入 `T042` exact failure-injection seam 范围。

当前 T040 结论：

- `tests/test_raft_segment_storage.cpp` 在 Windows 下改用更短的临时测试根路径；没有修改生产代码。
- 原始接收的 `6` 个 `RaftSegmentStorageTest` 平台无关红灯已在 focused rerun 全部转绿，并已从 failure matrix 删除。
- 本轮按执行规则未重跑 full managed；当前剩余项已全部收敛到 `T042` 的 `9` 个 exact seam deferred / non-equivalent 用例。

### T042 前置 Windows Exact Seam 收口结果

本次 exact seam 收口继续只处理剩余 `9` 个 Windows red lights，不扩大到
`T038` / `T039` / `T040` 已清零范围。

当前结果：

- `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'`
  - PASS
  - `49/49` 通过
- `ctest --preset windows-release-managed-tests --output-on-failure`
  - PASS
  - `104/104` 通过

本轮收口结论：

- 剩余 `9` 个 exact failure-injection seam 已在 Windows 下稳定触发并转绿。
- `windows-full-managed-failure-matrix.md` 已删除这 `9` 个 exact seam 项；
  当前 Windows full managed 无剩余失败项。
- `T042` 后续只负责跨平台文档回填与最终任务收口，不再承担新的失败修复。

## CTest Label Matrix

| Label | Meaning | Platform scope | Interpretation boundary |
|-------|---------|----------------|-------------------------|
| `platform-neutral` | 跨平台基础回归语义 | Linux / Windows / macOS fallback 均可用于逻辑回归 | 不自动声明 Linux-specific runtime 证据 |
| `platform-neutral-fallback` | 保守的跨平台 fallback baseline 子集 | 当前 Windows preset / PowerShell fallback 的默认执行范围 | 不代表整个 `platform-neutral` 语义桶都已在 Windows 纳入验收 |
| `durability-boundary` | restart / trusted-state / crash-style / durability boundary 语义 | 可通过 CTest 运行，但平台解释必须更谨慎 | 若同时带 Linux-specific 标签，则跨平台 fallback 只证明逻辑回归 |
| `linux-specific-failure-injection` | exact `fsync`、directory sync、replace/rename、prune/remove、partial write failure-injection 覆盖 | Linux-specific runtime evidence | Windows 侧 fallback 不声称等价注入语义 |
| `linux-primary-diagnosis` | Linux-primary diagnosis / retained-artifact / stress bucket | Linux-primary operational interpretation | Windows / 非 Bash fallback 不等价于该类诊断证据 |

Windows preset 过滤边界说明：

- `windows-release-tests` / `windows-debug-tests` 当前只运行保守 test-name
  子集：`CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、
  `ThreadPoolTest`。
- `windows-release-managed-tests` / `windows-debug-managed-tests` 不使用上述
  保守子集过滤，运行完整受管 CTest 目标集合。
- 由于当前粒度是 executable label 而不是单条 test case label，Windows
  fallback 采用保守子集策略：只有明确打上 `platform-neutral-fallback` 的
  语义才进入默认 preset 的解释范围；当前运行实现通过 test-name 子集落地。
- 因此 Windows baseline 只证明保守的 platform-neutral fallback 子集可运行，
  不声明“所有带有部分 platform-neutral 逻辑的 executable 都已在 Windows
  纳入验收”。
- Windows full managed 入口只是后续 sweep / 分类修复的承载入口，不得被写成
  Windows 已等价达到 Linux 当前 `104/104` 受管结果。

## `test.sh` Section Map

`test.sh` 的帮助输出和执行摘要必须明确展示以下 section，避免把
platform-neutral baseline、shared restart/durability 和 Linux-specific
解释边界混在一起。

| Section | Groups | Interpretation rule | Fallback |
|---------|--------|---------------------|----------|
| Platform-neutral base regression | `unit`, `snapshot-storage`, `kv-service`, `segment-basic`, `election`, `replication`, `integration`, `snapshot-catchup`, `snapshot-restart`, `replicator` | 基础逻辑回归属于平台无关 baseline；可在 Linux Bash 主入口低并发重跑，但不能因此误标记为 Linux-only 语义 | `ctest --preset debug-tests --output-on-failure` 或对应 `ctest -R` |
| Shared restart / durability regression | `persistence` | restart recovery / trusted-state 主体逻辑属于平台无关回归；若解释涉及 exact durability 或 retained artifacts，需同时引用 Linux-specific 边界 | `ctest --preset debug-tests --output-on-failure`，必要时回到 Linux Bash 主入口补充 retained-artifact 证据 |
| Linux-specific / Linux-primary focus | `snapshot-recovery`, `diagnosis`, `segment-cluster` | 当前主入口解释、时序风险和 retained-artifact 排障以 Linux Bash-first 为准；不得夸大为协议只在 Linux 正确 | 仅提供 logic fallback；不声称 Linux 等价 runtime 语义 |
| Linux Bash primary sweep | `all` | 执行顺序必须明确为 platform-neutral base regression -> `persistence` -> Linux-specific / Linux-primary focus -> final full-suite check | 非 Bash 环境改走 `ctest --preset debug-tests --output-on-failure` |

## Grouped Linux Rerun Guide

Use the following groups as the primary Linux rerun buckets for failure
localization. Unless a narrower command is required, prefer
`CTEST_PARALLEL_LEVEL=1` and add `--keep-data` when restart/snapshot/segment
artifacts are needed for diagnosis.

| Group | Primary purpose | Failure classification focus | Linux primary / Linux-specific | Minimal rerun command | When to add `--keep-data` | Platform-neutral fallback |
|------|------------------|------------------------------|-------------------------------|-----------------------|---------------------------|---------------------------|
| `snapshot-recovery` | Snapshot/restart recovery path | leader churn during recovery, snapshot restore failure, restart trusted-state mismatch | Linux primary hotspot | `CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-recovery` | When restart artifacts, snapshot dirs, or retained node data are needed | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftSnapshotRecoveryTest\.'` |
| `diagnosis` | Recovery diagnostics and snapshot fallback | snapshot skip/fallback, invalid snapshot rejection, restart explanation gaps | Linux primary hotspot | `CTEST_PARALLEL_LEVEL=1 ./test.sh --group diagnosis` | When snapshot skip/fallback evidence must be retained | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftSnapshotDiagnosisTest\.'` |
| `snapshot-catchup` | Lagging follower catch-up and snapshot handoff | follower catch-up gap, snapshot handoff sequencing, restart after catch-up | Linux primary grouped rerun | `CTEST_PARALLEL_LEVEL=1 ./test.sh --group snapshot-catchup` | When follower catch-up state or retained snapshot/log artifacts are needed | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftSnapshotCatchupTest\.'` |
| `replicator` | Single follower replication state machine | replication state drift, backoff, follower catch-up behavior | Linux primary grouped rerun | `CTEST_PARALLEL_LEVEL=1 ./test.sh --group replicator` | When retained per-node data helps explain replication drift | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftReplicatorBehaviorTest\.'` |
| `segment-cluster` | Clustered segment/snapshot stress path | segment rollover, clustered snapshot generation, retained-artifact stress failures | Linux primary grouped rerun | `CTEST_PARALLEL_LEVEL=1 ./test.sh --group segment-cluster` | When generated segment/snapshot trees must be inspected | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^RaftSegmentStorageTest\.RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory$'` |

Notes:

- `--keep-data` is a Linux Bash-first capability. It retains `raft_data/`,
  `raft_snapshots/`, and `build/linux/tests/raft_test_data/` for local diagnosis.
- `test.sh` must document `--keep-data` next to the section map and state that
  the first rerun step is `CTEST_PARALLEL_LEVEL=1 ./test.sh --group <name>`,
  with `--keep-data` added only when retained artifacts are needed.
- For Windows/macOS, the fallback entry remains
  `ctest --preset debug-tests --output-on-failure` or the corresponding direct
  `ctest --test-dir build/linux ... -R ...` command. On Windows, `.\test.ps1 -All`
  is the preferred one-command wrapper for the existing preset-based fallback.
  These fallbacks provide logic regression only and do not claim
  Linux-equivalent retained-artifact or crash-style runtime evidence.
- The rerun groups above are not a replacement for the full Linux primary path;
  they exist to narrow failures after a grouped run or a targeted investigation.

## Linux-Specific Boundaries

The following evidence is Linux-primary and must not be treated as completed
cross-platform runtime proof:

- Timing-sensitive validation of current flaky acceptance paths
- Exact durability failure injection around sync, directory sync, replace,
  prune, and partial-write boundaries
- Any future crash-style or process/signal-oriented failure harness
- Bash-first orchestration through `test.sh`
- Any executable or CTest-discovered case carrying the
  `linux-specific-failure-injection` or `linux-primary-diagnosis` label

For these areas, Windows/macOS follow-up must either:

- use the platform-neutral `ctest --preset debug-tests` path for logic-only
  regression, or
- on Windows, use `cmake --preset windows`,
  `cmake --build --preset windows-release`, and
  `ctest --preset windows-release-tests` for platform-neutral
  configure/build/test fallback without over-claiming Linux-specific
  semantics, or
- remain explicitly deferred until runtime validation exists.

### T031 Cross-Check Snapshot

The current document set now cross-checks the following Linux-specific /
crash-style / Bash-first areas against
`specs/004-raft-industrialization/platform-support.md`:

- T006 / T009:
  meta / log exact durability failure injection, including `fsync`,
  directory sync, replace/rename, partial write, and trusted-state boundary
  expectations, remains Linux-specific runtime evidence only.
- T007 / T010:
  snapshot publish / prune failure injection, including temp-dir publish,
  replace/rename, prune/remove, and snapshot durability-boundary behavior,
  remains Linux-primary evidence only.
- T011:
  injected-failure diagnostics, retained-artifact diagnosis, and crash-style
  failure localization remain Linux-primary and are not rewritten as Windows
  runtime proof.
- T023:
  `./test.sh` plus low-parallel grouped reruns and `--keep-data` remain the
  Linux Bash primary path; Windows preset / PowerShell flows are documented as
  fallback only.
- T024 / T025:
  Windows preset fallback and `.\test.ps1 -All` are explicitly limited to a
  conservative platform-neutral baseline subset, not full Raft acceptance.
- T026:
  `platform-neutral`, `durability-boundary`,
  `linux-specific-failure-injection`, and `linux-primary-diagnosis` label
  boundaries remain consistent with the Linux-primary interpretation rules.
- T028 / T030:
  `tests/persistence_more_test.cpp` and `tests/test_temp.cpp` are documented as
  manual-only / diagnostic-only / temporary assets and are not interpreted as
  formal cross-platform acceptance artifacts.

For all rows above:

- Windows fallback remains non-equivalent to Linux-specific durability /
  failure-injection / crash-style acceptance.
- macOS remains out of validation scope for this feature and is only mentioned
  as deferred / not validated.

## US1 Accepted Restart Recovery Evidence

| Evidence area | Covered scenarios | Entrypoint | Latest status | Platform scope |
|---------------|-------------------|------------|---------------|----------------|
| Hard-state and log-boundary restart matrix | old-meta/new-log, new-meta/old-log, commit/apply clamp, missing first segment, final segment tail truncate 后 trusted log prefix 选择 | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest)\.'` | PASS | 平台无关恢复逻辑证据；如涉及 exact durability 边界，则由 Linux-specific 注入测试补充 |
| Snapshot metadata and applied-state restart matrix | invalid snapshot rejection, all-invalid fallback, metadata mismatch, corrupted newest snapshot fallback, trusted snapshot 选择后一致的 applied replay | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^(RaftSnapshotRecoveryTest|RaftSnapshotDiagnosisTest)\.'` | PASS | 平台无关恢复逻辑证据 |
| Linux-specific durability failure injection and diagnostics | meta/log/snapshot 的 file sync、directory sync、replace/rename、prune/remove、partial write 边界及对应 trusted-state 预期 | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'` | PASS | Linux-specific runtime evidence；对应 executable 在 CTest 中应带 `linux-specific-failure-injection` 与 `durability-boundary` 标签；Windows 侧仅保留 platform-neutral fallback，不声称等价注入语义 |

Current US1 status:

- T012 与 T013 的 restart matrix 已纳入受管 GTest / CTest 回归。
- T014 已修复 restart trusted-state / final-segment trusted-prefix 恢复缺口。
- 当前没有遗留 tests-first 红灯；US1 restart recovery 进入 regression-only 状态。

## US2 Accepted Consistency Evidence

| Evidence area | Covered scenarios | Entrypoint | Latest status | Platform scope |
|---------------|-------------------|------------|---------------|----------------|
| Catch-up and snapshot handoff consistency | follower 落后 live log 后通过 log replay catch-up 恢复；follower 落后到 retained snapshot boundary 后通过 snapshot handoff catch-up 恢复；catch-up 后 committed ordering 保持一致 | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^(RaftSnapshotCatchupTest|RaftIntegrationTest)\.'` | PASS | 平台无关 cluster consistency 逻辑证据 |
| Leader-switch and commit/apply ordering consistency | leader 切换后 committed state 保持不变；新 leader 继续推进新日志；lagging follower、leader switch 与新 proposal 混合时 commit/apply 不逆序 | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^(RaftLeaderSwitchOrderingTest)\.'` | PASS | 平台无关 cluster consistency 逻辑证据 |
| State-machine replay consistency | snapshot load 后 tail replay 不丢失；restart 后 committed log apply 一致；state machine 最终视图与 committed/applied 状态一致；duplicate apply / missed apply 风险受回归保护 | `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R '^(KvStateMachineTest|RaftSnapshotRecoveryTest)\.'` | PASS | 平台无关 replay / restart consistency 逻辑证据 |

Current US2 status:

- T016 已提供 catch-up / snapshot handoff 回归证据，当前没有暴露 `replicator.cpp` 生产缺口。
- T017 已提供 leader-switch / commit-apply ordering 回归证据，当前没有暴露 `raft_node.cpp` 生产缺口。
- T018 已提供 state-machine replay consistency 回归证据，当前没有暴露 `state_machine.cpp` 或 `raft_node.cpp` 生产缺口。
- T019、T020、T021 均以 no-op 关闭：因为对应 tests-first 回归均通过，当前没有进入生产修复的输入证据。
- 当前没有 US2 tests-first 红灯；US2 consistency 进入 regression-only 状态。
- US2 完成后，lag/restart/leader-switch 一致性已具备独立验收所需的回归证据，不依赖新增协议语义或大范围生产代码重写。

### US2 Platform-Neutral Regression Coverage

- catch-up after lag:
  follower 落后 live log 后通过 log replay catch-up 恢复，以及 follower 落后到 retained snapshot boundary 后通过 snapshot handoff catch-up 恢复，均已由 `RaftSnapshotCatchupTest` / `RaftIntegrationTest` 受管 CTest 覆盖。
- leader switch consistency:
  leader 切换后 committed state 保持不变、新 leader 继续推进 committed log、lagging follower 与新 proposal 混合情况下状态仍能收敛，均已由 `RaftLeaderSwitchOrderingTest` 覆盖。
- commit/apply ordering:
  `commit_index` 与 `last_applied` 的顺序边界，以及 leader switch / follower catch-up 交织时不出现 commit/apply 逆序，已由 `RaftLeaderSwitchOrderingTest` 和相关 US2 回归覆盖。
- state-machine replay consistency:
  snapshot load 后 tail replay、restart 后 committed log apply、state machine 最终视图与 committed/applied 状态一致，以及 duplicate apply / missed apply 风险，已由 `KvStateMachineTest` / `RaftSnapshotRecoveryTest` 覆盖。

### US2 Linux-Primary Observation Or Follow-Up Risk

- timing-sensitive clustered validation:
  虽然 US2 语义已由平台无关回归覆盖，但更长链路、时序更敏感的集群稳定性观察仍主要依赖 Linux 主验收入口，例如 `./test.sh --group snapshot-catchup`、`replicator`、`replication` 的低并发复验。
- retained-artifact diagnosis:
  若后续需要保留 `raft_data/`、`raft_snapshots/` 或构造现场进行人工诊断，当前仍以 Linux Bash-first 的 `--keep-data` 流程为主；这属于诊断便利性差异，不影响已记录的 US2 平台无关逻辑证据。
- cross-platform runtime follow-up:
  Windows/macOS 目前仍以 `ctest --test-dir build/linux ...` 的平台无关逻辑回归为主，尚未补充与 Linux 主验收等价的长链路运行时观察；这仍属于 `W6/W8` 的后续范围，而不是 US2 未完成项。

## Current Risk Register

| ID | Risk | Current classification | Evidence | Follow-up task area |
|----|------|------------------------|----------|---------------------|
| R1 | Snapshot recovery timing assumption causes false negative acceptance failures | Complete but consistency-risky | `003 progress.md` blocked item 1 | T003, T005 |
| R2 | Split-brain timing assumption causes false negative acceptance failures | Complete but consistency-risky | `003 progress.md` blocked item 2 | T004, T005 |
| R3 | Exact sync/rename/prune failure boundaries are not covered by deterministic tests | Half-finished / missing industrialization ability | `003 progress.md` blocked T613-T616 | T006-T011 |
| R4 | Restart trusted-state combinations for term/vote/log/snapshot metadata/applied state regress | Accepted for US1 and protected by managed restart matrix tests | T012/T013 targeted matrices plus T014 recovery fixes | Regression only; keep evidence in T015 |
| R5 | Catch-up, leader switch, and replay consistency need stronger regression evidence | Accepted for US2 and protected by managed consistency regressions | T016/T017/T018 targeted regressions passed; T019/T020/T021 were no-op because no production defect evidence was proven | Regression only; documentation follow-up stays in T022 |
| R6 | Current validation flow is useful on Linux but not yet consolidated cross-platform | Cross-platform risk | `test.sh`, `CMakePresets.json`, `plan.md` W6 | T023-T029 |
| R7 | Windows/macOS runtime semantics for durability-related paths are still not verified | Cross-platform risk / deferred | `003 progress.md` notes, `spec.md` platform scope | T027, T031, T032 |

## Completion Classification Snapshot

### Judged Completed

- Stable Raft core behaviors already listed as protected baseline
- Segment log durability hardening from `003`
- `meta.bin` durability hardening from `003`
- Snapshot staged atomic publish hardening from `003`
- Restart recovery diagnostic strengthening from `003`
- Constructed crash-artifact recovery coverage already added in `003`
- Former manual two-phase recovery scenarios from `tests/persistence_more_test.cpp`
  are now covered by managed CTest via
  `RaftSnapshotDiagnosisTest.RestartedSingleNodeLoadsSnapshotAndTailLogsWithoutPeers`
  and
  `RaftSnapshotDiagnosisTest.CompactedClusterReplicatesNewLogAfterRestartedLeaderStepsDown`
- Restart trusted-state and log-boundary matrix coverage from T012/T014 via
  `PersistenceTest` and `RaftSegmentStorageTest` targeted restart matrices
- Snapshot metadata and applied-state restart matrix coverage from T013 via
  `RaftSnapshotRecoveryTest` and `RaftSnapshotDiagnosisTest`
- Catch-up and snapshot handoff consistency coverage from T016 via
  `RaftSnapshotCatchupTest` and `RaftIntegrationTest`
- Leader-switch and commit/apply ordering consistency coverage from T017 via
  `RaftLeaderSwitchOrderingTest`
- State-machine replay consistency coverage from T018 via
  `KvStateMachineTest` and `RaftSnapshotRecoveryTest`
- T019/T020/T021 concluded as no-op because T016/T017/T018 did not produce
  tests-first red evidence requiring production fixes
- US3 validation-entry documentation from T023-T029, including Linux primary
  path, Windows preset fallback, PowerShell fallback, label boundaries,
  platform-support matrix, quickstart / tests README guidance, and final
  interpretation rules for current PASS/FAIL status
- Current US1 restart recovery acceptance has no remaining tests-first red
  cases in the managed CTest path
- Current US2 consistency acceptance has no remaining tests-first red cases in
  the managed CTest path

### Enters Follow-Up Hardening

- Linux flaky acceptance stabilization
- Exact failure injection seams and tests
- Cross-platform validation entry consolidation
- Platform support and failure-localization documentation
- Windows/macOS runtime-validation and CI follow-up definition

## Notes

- No root-level `phase-reports/` artifact is currently available for this scope.
  Carry-forward completion status therefore uses
  `specs/003-persistence-reliability/progress.md` as the authoritative phase
  baseline.
- `tests/persistence_more_test.cpp` is retained as a manual-only /
  diagnostic-only helper for two-phase rerun demos, marker files, and exported
  text snapshots/manifests. It is not part of CTest because it depends on
  re-running the same executable across phases and human inspection of retained
  artifacts.
- `tests/test_temp.cpp` is currently treated as a temporary test file. Although
  it uses GTest form, it is not registered by `tests/CMakeLists.txt`, does not
  participate in managed CTest regression, and must not be interpreted as a
  formal acceptance asset. Any cleanup, deletion, or migration stays in
  follow-up work and is not executed in this scope.
- This document does not authorize any production-code rewrite. It only freezes
  scope and validation expectations for the remaining industrialization work.
