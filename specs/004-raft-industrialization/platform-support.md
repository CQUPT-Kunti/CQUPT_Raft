# Platform Support Matrix

## 目标

记录 `004-raft-industrialization` 当前在 Linux / Windows 的平台支持状态、
验证入口、已验证范围、未验证范围和后续 follow-up，避免把
Windows conservative baseline、Windows full managed CTest sweep、
cluster/runtime-heavy、durability-boundary、linux-specific-failure-injection
混为一谈。

本文件只记录当前证据和解释边界，不改变 CQUPT_Raft 的协议语义、持久化格式、
公共 API 或测试行为。

## 范围说明

- Linux：当前主验证平台
- Windows：当前提供保守 fallback 验证平台
- macOS：当前不在本 feature 验证范围内

## 术语边界

### platform-neutral baseline

- 可用于跨平台逻辑回归的保守基线
- 当前 Windows 默认只声明这一层的保守子集，不代表 Raft 全功能都已验证

### Windows full managed CTest sweep

- 指 Windows 下完整受管 CTest 目标集合的单独 sweep 入口
- 它不同于 conservative baseline
- 它的存在只表示“入口已提供”，不自动表示“已经通过”

### cluster/runtime-heavy

- 需要更长链路运行时行为、集群协同、时序稳定性或更高操作负载的回归
- 当前主要仍以 Linux 主环境解释

### durability-boundary

- restart / trusted-state / crash-style / durability boundary 语义
- 这类能力即使可以通过 CTest 运行，也不自动等价为跨平台运行时证据

### linux-specific-failure-injection

- exact `fsync`、directory sync、replace/rename、prune/remove、partial write
  等精确 failure-injection 语义
- 当前只记录 Linux 主环境证据

## 平台支持矩阵

| 平台 | 当前角色 | 主验证入口 | 当前状态 | 已验证范围 | 未验证范围 / 不声明事项 |
|------|----------|------------|----------|------------|-------------------------|
| Linux | 主验证平台 | `./test.sh` / `ctest --preset debug-tests` | low-parallel configure/build 已记录 PASS；Linux 受管 CTest 当前 `104/104` PASS | Linux Bash 主入口、受管 CTest、`platform-neutral` 100 个测试、`durability-boundary` 4 个测试、Linux-specific durability / failure-injection / crash-style 解释边界 | 不把 Linux 结果外推为 Windows 等价覆盖；Windows Raft 全功能仍是 follow-up |
| Windows | 保守 fallback 平台，并提供 full managed CTest 入口 | `.\test.ps1 -All` / `ctest --preset windows-release-managed-tests` | conservative baseline 已记录 PASS；最近一次 full managed 正式 rerun 已 `104/104 PASS` | Visual Studio 2022 MSVC x64 下的保守 platform-neutral baseline 子集：`CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`；以及 Windows full managed CTest 全量 `104` 个受管测试最新 rerun 全绿 | 不把 Windows 结果写成“等价 Linux-specific durability 语义”；macOS 仍不在当前验证范围内 |
| macOS | 不在本 feature 验证范围内 | N/A | 当前不在本 feature 验证范围内 | 无 | 不写已验证，不声明任何等价运行时证据 |

## 各平台验证入口

### Linux

- configure preset：`debug-ninja-low-parallel`
- build preset：`debug-ninja-low-parallel`
- CTest fallback：`debug-tests`
- Bash 主入口：`./test.sh`

当前记录的结果：

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel`：PASS
- `./test.sh --group persistence`：PASS
- `ctest --preset debug-tests --output-on-failure`：PASS
- Linux 受管 CTest 当前 `104/104` 通过
- Label 统计：`platform-neutral` 100 个测试，`durability-boundary` 4 个测试

解释规则：

- Linux 是当前唯一主验收平台。
- Linux-specific durability / failure-injection / crash-style 语义继续以
  `./test.sh` 主入口解释。
- `ctest --preset debug-tests --output-on-failure` 在 Linux 上仍是 CTest
  fallback；当前结果已全绿，但它仍不改变 Linux-specific 语义的解释边界。
- 当前不再记录 `debug-tests` / cluster-runtime 红灯。

### Windows

- generator / toolchain：Visual Studio 17 2022 / MSVC x64
- configure preset：`windows`
- build preset：`windows-release`
- test preset：`windows-release-tests`
- full managed test preset：`windows-release-managed-tests`
- optional debug managed preset：`windows-debug-managed-tests`
- PowerShell fallback：`test.ps1`

当前记录的结果：

- `cmake --preset windows`：PASS
- `cmake --build --preset windows-release`：PASS
- `ctest --preset windows-release-tests`：PASS
- `ctest --preset windows-release-managed-tests`：PASS
  - 最近一次正式 full managed rerun 记录为 `104` 个受管测试全部通过
- `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'`：PASS
  - focused exact seam rerun 当前 `49/49` 通过

解释规则：

- Windows 当前只提供保守的 platform-neutral fallback。
- Windows 现在额外提供 full managed CTest sweep 入口，但它不是默认入口。
- `test.ps1` 只是对 `windows` / `windows-release` /
  `windows-release-tests` 的一键封装。
- `.\test.ps1 -Managed` 是对 `windows` / `windows-release` /
  `windows-release-managed-tests` 的显式 full managed 封装。
- Windows 默认覆盖的 test-name 子集是：
  `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、
  `ThreadPoolTest`。
- 这不代表所有带有部分 `platform-neutral` 逻辑的受管测试都已在 Windows
  进入默认验收范围。
- `windows-release-managed-tests` 当前已在 Windows 下完成一次 `104/104 PASS`
  的正式 rerun，但这不改变 Linux 仍是 Linux-specific durability /
  failure-injection 语义的主解释平台这一边界。
- `T041` 已修掉 Windows 目录句柄权限导致的
  `FlushFileBuffers ... GetLastError=5` 主信号；后续 exact seam 也已在 Windows
  rerun 中转绿。

### Windows full managed 结果快照

本次 `T034` 直接复用 `T033` 的两份日志：

- `tmp/windows-release-managed-tests.log`
- `tmp/test-ps1-managed.log`

首次 sweep 的历史结果仍保留用于问题分流；当前最新正式记录为：

- conservative baseline：
  - `windows-release-tests`
  - `PASS`
  - 当前仍然只覆盖 `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、
    `ThreadPoolTest`
- full managed 首次 sweep：
  - `windows-release-managed-tests`
  - `FAIL`
  - 历史首轮曾暴露 `85` 个失败项
- full managed 最新 rerun：
  - `windows-release-managed-tests`
  - `PASS`
  - 当前 `104/104` 通过
- focused exact seam 最新 rerun：
  - `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'`
  - `PASS`
  - 当前 `49/49` 通过

当前平台摘要只保留到这里；完整失败测试名、失败分类矩阵和 19 个受管目标的
PASS / FAIL / BLOCKED 状态，统一收敛到：

- [windows-full-managed-failure-matrix.md](./windows-full-managed-failure-matrix.md)

当前 failure matrix 的任务分流摘要：

- `T036`：已确认当前没有独立的 preset / discover / working directory /
  multi-config / test filter / wrapper 阻塞，可按 no-op 处理
- `T037`：已对 `raft_integration_test.cpp` 收紧 Windows 长路径 harness 假设；
  当前独立 runtime/harness 项为 `0`，原先 7 项已转交 `T041`
- `T038`：已收口；原始 4 项 election / replication / commit-apply 红灯已转绿
- `T039`：已收口；当前无独立剩余 snapshot / restart / catch-up 红灯
- `T040`：已收口；原始 `6` 项 platform-neutral persistence / segment / storage 红灯已转绿
- `T041`：已修掉 Windows 目录 flush 句柄权限问题，并完成 exact seam Windows rerun 收口
- `T042`：后续只剩跨平台文档回填与最终任务收口，不再承担剩余失败修复

## 验证范围分层

| 分类 | Linux 当前记录 | Windows 当前记录 | macOS 当前记录 |
|------|----------------|------------------|----------------|
| platform-neutral baseline | Linux 受管 CTest 当前 `104/104` PASS，其中 `platform-neutral` 100 个测试通过 | 只记录保守 baseline 子集已通过 | 当前不在本 feature 验证范围内 |
| Windows full managed CTest sweep | Linux 侧已有完整受管 CTest 作为对齐参照 | 已有单独入口，且最近一次正式 rerun 已 `104/104 PASS`；但不把该结果写成 Linux-specific 语义等价证明 | 当前不在本 feature 验证范围内 |
| cluster/runtime-heavy | 当前 Linux 受管 CTest 未再记录红灯；更长时间 soak / stress 仍可作为后续观察 | 不声明已覆盖，也不声明已通过 | 当前不在本 feature 验证范围内 |
| durability-boundary | 已记录 Linux 主环境解释与相关恢复边界 | 不声明等价运行时验证 | 当前不在本 feature 验证范围内 |
| linux-specific-failure-injection | 当前只记录 Linux 证据 | 明确不等价、不继承 | 当前不在本 feature 验证范围内 |

## Linux-specific 任务交叉检查

下表用于把 `T006/T007/T011/T023-T030` 中涉及 Linux-specific failure
injection、crash-style、Bash-first 与 durability-boundary 的内容，同当前平台
支持边界逐项对齐。

| 任务 / 内容 | Linux 当前解释 | Windows 当前 fallback | 未等价验证 / deferred 说明 | macOS |
|------------|----------------|-----------------------|----------------------------|-------|
| T006 / T009：meta / log failure injection | exact `fsync`、directory sync、replace/rename、partial write、trusted-state 边界只记为 Linux-specific runtime evidence | 只允许回退到 `ctest --preset debug-tests` 的 logic fallback，或 Windows preset / `test.ps1` 的保守 platform-neutral baseline | 不声明 Windows 已完成等价 durability 运行时验证；后续继续保留 Windows durability 语义适配与 runtime validation follow-up | 当前不在本 feature 验证范围内 |
| T007 / T010：snapshot publish / prune failure injection | temp-dir publish、replace/rename、prune/remove、snapshot durability boundary 继续由 Linux 主验收解释 | 只允许回退到 Linux `debug-tests` logic fallback，或 Windows preset / `test.ps1` 的保守 baseline；不把 publish/prune 注入语义转写为 Windows 已验证 | Windows snapshot publish/prune 等价语义仍属 deferred / follow-up，不记录为已验证 | 当前不在本 feature 验证范围内 |
| T011：injected-failure diagnostics | retained-artifact、diagnosis、crash-style 失败定位继续按 Linux-primary 解释 | Windows 只保留 platform-neutral fallback，不提供与 `--keep-data` 或 Linux-primary diagnosis 等价的排障能力 | Windows retained-artifact / diagnosis / crash-style 运行时观察继续保留为 follow-up | 当前不在本 feature 验证范围内 |
| T023：Linux Bash 主入口 | `./test.sh`、低并发分组和 `--keep-data` 是 Linux 主验收路径 | Windows fallback 入口是 preset + `.\test.ps1 -All`；Linux 的 `ctest --preset debug-tests` 只是 CTest fallback | 不把 Windows preset / PowerShell fallback 写成 Bash-first 等价入口 | 当前不在本 feature 验证范围内 |
| T024 / T025：Windows preset / PowerShell fallback | Linux 主入口语义不变 | `cmake --preset windows`、`cmake --build --preset windows-release`、`ctest --preset windows-release-tests`、`.\test.ps1 -All` 只代表保守 platform-neutral baseline | 不声明 Windows Raft 全功能通过；cluster-style、durability、failure-injection 继续 deferred | 当前不在本 feature 验证范围内 |
| T033：Windows full managed CTest 入口 | Linux 当前 `104/104` 作为完整受管 CTest 对齐参照 | `ctest --preset windows-release-managed-tests`、`ctest --preset windows-debug-managed-tests`、`.\test.ps1 -Managed` 只表示 full managed 入口已单独提供 | 不声明 Windows full managed 已通过；不声明与 Linux `104/104` 等价；不把 Linux-specific durability 写成 Windows 已验证 | 当前不在本 feature 验证范围内 |
| T026：CTest label / Linux-specific 边界 | `platform-neutral`、`durability-boundary`、`linux-specific-failure-injection`、`linux-primary-diagnosis` 当前都按 Linux 主环境边界解释 | Windows fallback 只消费保守 baseline 子集，不因为 label 存在就继承 Linux-specific 语义 | mixed executable 不会被写成 Windows 已等价验证；Linux-specific 语义仍保留为 deferred runtime follow-up | 当前不在本 feature 验证范围内 |
| T028 / T030：manual-only / diagnostic-only / temporary 文件 | `persistence_more_test.cpp`、`test_temp.cpp` 不属于 Linux 主回归入口，只作为手工诊断或临时文件说明 | 不进入 Windows platform-neutral fallback，也不作为跨平台正式验收资产 | 若后续清理或迁移，仅作为 follow-up 记录，不在当前范围执行 | 当前不在本 feature 验证范围内 |

## 已验证范围

### Linux 已验证范围

- `debug-ninja-low-parallel` configure/build 入口可用
- `./test.sh` 作为 Linux 主入口仍可用
- `./test.sh --group persistence` 仍可用
- `ctest --preset debug-tests --output-on-failure` 当前 `104/104` PASS
- `platform-neutral` 100 个测试通过
- `durability-boundary` 4 个测试通过
- `tests/CMakeLists.txt` 中 `platform-neutral`、`durability-boundary`、
  `linux-specific-failure-injection`、`linux-primary-diagnosis` 的标签语义
  与文档解释保持一致
- Linux-specific durability / failure-injection / crash-style 语义仍由 Linux
  主环境负责解释

### Windows 已验证范围

- Visual Studio 2022 MSVC x64 下的 preset configure/build/test fallback 可用
- `test.ps1` 可作为 Windows PowerShell fallback 入口
- 当前默认只覆盖保守 platform-neutral baseline 子集：
  `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`
- 已新增 Windows full managed CTest 入口：
  `windows-release-managed-tests`、`windows-debug-managed-tests`、`.\test.ps1 -Managed`
- Windows full managed 当前状态：
  - `ctest --preset windows-release-managed-tests` 最近一次正式 rerun 已 `104/104 PASS`
  - `ctest --test-dir build/windows -C Release --output-on-failure -R '^(PersistenceTest|RaftSegmentStorageTest|SnapshotStorageReliabilityTest|RaftSnapshotRecoveryTest)\.'` 最近一次 focused exact seam rerun 已 `49/49 PASS`
- `T038` / `T041` rerun 已确认：
  `RaftElectionTest.FollowerRejectsClientProposeAfterLeaderIsElected`
  、`RaftLeaderSwitchOrderingTest.CommittedStateSurvivesLeaderSwitchAndNewLeaderContinuesReplication`
  、`RaftLeaderSwitchOrderingTest.LaggingFollowerCatchesUpDuringLeaderSwitchWithoutCommitApplyReordering`
  、`RaftReplicatorBehaviorTest.SlowFollowerCatchesUpWhileLeaderKeepsAcceptingNewLogs`
  已转绿
- `T039` rerun 已确认：
  `RaftSnapshotCatchupTest.*`、`RaftSnapshotRestartTest.*`、
  `RaftSnapshotDiagnosisTest.*` 与 platform-neutral 的
  `RaftSnapshotRecoveryTest.*` snapshot / restart / catch-up 本体断言已转绿；
  唯一剩余 snapshot 相关红灯为 exact seam 项
  `RaftSnapshotRecoveryTest.RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam`，
  已转交 `T042`
- `T041` focused rerun 已确认：
  `RaftKvServiceTest.*` 与 `RaftIntegrationTest.*` 中先前被
  `FlushFileBuffers ... GetLastError=5` 阻塞的 7 个用例已转绿

## 未验证范围

### Linux 后续观察范围

- 当前 Linux 受管 CTest 已全绿，不再记录 `debug-tests` / cluster-runtime 红灯。
- 若需要进一步增强工业化可信度，后续仍可补充更长时间的 soak、stress、
  retained-artifact 和 crash-style 验证。
- Linux-specific durability / failure-injection 仍只代表 Linux 主环境证据，
  不自动外推到 Windows。

### Windows 未验证范围

- Windows cluster-style 测试扩大范围
- Windows full managed `104/104 PASS` 之后的长期 soak / CI 持续验证
- Windows 与 Linux 在更深层 crash-style / retained-artifact 运维流程上的完全等价性证明
- Windows 下 Bash-first retained-artifact / `--keep-data` 等价流程

## 路径、临时目录与进程模型说明

### Linux

- 当前主验证流程基于 Bash：`./test.sh`
- retained-artifact 与 `--keep-data` 诊断流程只在 Linux 主入口解释
- durability 相关的 flush/sync、rename/replace、temp-dir publish/prune、
  crash-style 语义继续按 Linux 主环境记录

### Windows

- 当前文档化入口基于 Visual Studio preset 与 PowerShell wrapper
- Windows full managed 当前已完成一次 `104/104 PASS` 的正式 rerun
- 路径、临时目录、rename/replace、flush/sync 与 crash-style 语义仍不写成
  “与 Linux 主验收完全等价”的证据

## 后续 Follow-Up

- 扩大 Windows cluster-style 测试覆盖范围，而不是停留在保守 baseline 子集
- 补齐 Windows 下的 Raft election / replication / snapshot / persistence
  稳定性验证
- 评估并记录 Windows durability 语义适配，包括 flush/sync、rename/replace、
  temp-dir publish/prune 与 crash-style 边界
- 扩展 CI matrix，使 Linux 主验证与 Windows fallback 的边界和结果可持续复现
- 若未来引入新的非 Bash 平台入口，必须继续保持“不虚报 Linux-specific
  等价证据”的文档约束

## 结论

- Linux 仍是当前主验证平台。
- Windows conservative baseline 与 Windows full managed 入口都已可用，且最近一次
  `windows-release-managed-tests` 已 `104/104 PASS`。
- Windows 结果不等价于 Linux-specific durability / failure-injection /
  crash-style 语义主解释。
- 当前 `US3` 的入口文档和状态说明已进入最终收口阶段；后续主要剩长期验证和 CI
  扩展 follow-up。
- 当前不记录 macOS 已验证；macOS 仍不在本 feature 验证范围内。
