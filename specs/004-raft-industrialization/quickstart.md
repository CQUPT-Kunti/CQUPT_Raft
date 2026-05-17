# Quickstart: CQUPT_Raft 工业化跨平台完善

## 1. 先读范围边界

- 功能范围与非目标：[spec.md](./spec.md)
- 执行顺序与验收标准：[plan.md](./plan.md)
- 风险冻结与验证矩阵：[validation-matrix.md](./validation-matrix.md)
- 平台支持矩阵：[platform-support.md](./platform-support.md)
- 编码注意事项速查：[cross-platform-coding-notes.md](./cross-platform-coding-notes.md)

`validation-matrix.md` 是本 feature 的单一验证基线。后续执行、rerun、平台解释和
Linux-specific 边界，都以它为准。

## 2. 当前推荐的验证顺序

1. 先阅读 [validation-matrix.md](./validation-matrix.md)，确认哪些能力已完成，
   哪些属于后续补强，哪些是 Linux-specific，哪些仍是 Windows/macOS deferred。
2. 使用 Linux 主入口完成低并发构建和主回归。
3. 若某个高风险区域失败，按矩阵中的 focused rerun 命令进入对应分组。
4. 若当前环境是 Windows，优先使用 `.\test.ps1 -All` 完成 PowerShell
   platform-neutral fallback。
5. 若需要单独观察 Windows 的完整受管 CTest sweep，显式运行
   `ctest --preset windows-release-managed-tests` 或 `.\test.ps1 -Managed`。
6. 若不在 Linux 上且不适用 Windows preset，或只需要跑平台无关基线，使用
   `ctest --preset debug-tests` 作为 fallback。

## 3. Linux 主入口怎么跑

当前 Linux 主入口是：

```bash
./test.sh
```

推荐一律配合低并发：

```bash
CTEST_PARALLEL_LEVEL=1
```

先构建：

```bash
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel
```

再执行 Linux 主回归入口：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all
```

这是当前 Linux 主验证路径，与
[validation-matrix.md](./validation-matrix.md) 中的 `Linux primary` 定义保持一致。

当前 Linux 已记录的状态：

- `cmake --preset debug-ninja-low-parallel`：PASS
- `cmake --build --preset debug-ninja-low-parallel`：PASS
- `./test.sh --group persistence`：PASS
- `ctest --preset debug-tests --output-on-failure`：PASS
- Linux 受管 CTest 当前 `104/104` 通过
- Label 统计：`platform-neutral` 100 个测试，`durability-boundary` 4 个测试

解释规则：

- `persistence` 当前已经通过，可作为 Linux restart / durability 主回归入口之一。
- `debug-tests` 当前已全绿，可作为 Linux CTest fallback 和受管测试总入口。
- Linux-specific durability / failure-injection / crash-style 语义仍以
  `./test.sh` 主入口解释。
- Windows fallback 仍只是保守 baseline，不因为 Linux `debug-tests` 全绿而扩大为
  Windows Raft 全功能通过。

## 4. 失败后先看哪里

如果 Linux 主入口失败，先不要直接扩大范围，优先按原 group 低并发重跑：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group <group>
```

需要 retained artifacts 时，再追加：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group <group> --keep-data
```

失败后优先查看：

- [validation-matrix.md](./validation-matrix.md)
  中的 group 解释、rerun 命令和 Linux-specific 边界
- [platform-support.md](./platform-support.md)
  中的平台支持范围与 Windows fallback 边界
- [tests/README.md](../../tests/README.md)
  中的测试角色说明：哪些是受管回归、哪些是 manual-only /
  diagnostic-only、哪些不进入 Windows fallback

## 5. 如何使用 `--keep-data` 保留现场

当需要保留失败现场、测试数据目录或辅助排障时，使用：

```bash
CTEST_PARALLEL_LEVEL=1 ./test.sh --group all --keep-data
```

高风险分组的 rerun 也可以保留数据，例如：

```bash
./test.sh --group snapshot-recovery --keep-data
./test.sh --group diagnosis --keep-data
./test.sh --group persistence --keep-data
```

解释规则：

- `--keep-data` 只属于 Linux Bash 主入口能力。
- 平台无关 `ctest --preset debug-tests` fallback 不默认提供等价的数据保留语义。
- 如果需要 retained artifacts 进行排障，应优先回到 Linux 主入口重跑。

重点保留的目录语义是：

- `raft_data/`
- `raft_snapshots/`
- `build/linux/tests/raft_test_data/`

## 6. 高风险区域的真实 rerun 命令

这些命令来自当前 `test.sh` 分组，必须与
[validation-matrix.md](./validation-matrix.md) 保持一致：

```bash
./test.sh --group persistence
./test.sh --group snapshot-recovery
./test.sh --group diagnosis
./test.sh --group snapshot-catchup
./test.sh --group replicator
./test.sh --group replication
./test.sh --group election
```

推荐的解释方式：

- `persistence`：重启恢复、hard state、log trusted-state 边界
- `snapshot-recovery`：snapshot/restart 路径，当前也是 Linux flaky 风险热点
- `diagnosis`：恢复诊断、snapshot skip/fallback、边界错误定位
- `snapshot-catchup`：follower 落后后通过 log replay 或 snapshot handoff 追赶
- `replicator`：单 follower 复制状态机与 catch-up 行为
- `replication`：日志复制与 commit/apply 主路径回归
- `election`：选举与 split-brain 相关回归

## 7. 平台无关 CTest fallback

如果当前环境不走 Bash 主入口，或只需要执行平台无关的基础回归，使用：

```bash
ctest --preset debug-tests --output-on-failure
```

解释规则：

- 这是当前唯一明确的跨平台 baseline fallback。
- 它适合平台无关的逻辑回归，不自动等价于 Linux-specific crash-style 或 failure-injection 证据。
- 当 `validation-matrix.md` 中某项被标记为 Linux-specific 时，`ctest --preset debug-tests`
  只能作为逻辑回归 fallback，不能替代 Linux runtime 语义验证。
- 若需要进一步判断命中的受管测试是否包含 Linux-specific
  failure-injection / diagnosis 语义，应回看 `tests/CMakeLists.txt` 中的
  CTest label 约定，以及 `validation-matrix.md` 的标签解释。
- 当前 Linux 上的 `debug-tests` 已全绿，当前记录为 `104/104` tests passed；
  其中 `platform-neutral` 100 个测试、`durability-boundary` 4 个测试通过。

## 8. Windows fallback 怎么跑

如果当前环境是 Windows，优先使用：

```bash
.\test.ps1 -All
```

解释规则：

- `test.ps1` 是 Windows PowerShell fallback wrapper。
- `windows` configure preset 保持现有 Visual Studio 17 2022 multi-config 配置不变。
- `windows-release` 是当前 Windows Release 构建入口。
- `windows-release-tests` 只代表 Windows Release 的 platform-neutral fallback，
  当前只运行保守 test-name 子集：
  `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`。
- 如需额外验证 Debug 路径，使用 `windows-debug` 与 `windows-debug-tests`。
- `windows-release-managed-tests` 是单独的 Windows Release full managed CTest
  sweep 入口，不会替换掉 `windows-release-tests`。
- 如需 Debug full managed sweep，可使用 `windows-debug-managed-tests`。
- `platform-neutral-fallback` 比完整的 `platform-neutral` 语义桶更保守。
- 若某个 executable 没有 `platform-neutral-fallback`，即使它仍带有
  `platform-neutral` 语义，也不代表它已经被纳入 Windows 默认 fallback 验证。
- `.\test.ps1 -All` 的底层等价流程仍然是：

```bash
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release-tests
```

- 这组命令不等价于 Linux-specific crash-style、failure-injection、
  directory sync 或 `--keep-data` 证据。
- 不声明 Windows Raft 全功能通过。
- 不声明 Windows 已等价验证 Linux-specific durability /
  failure-injection。

如果需要显式触发 Windows full managed CTest sweep，使用：

```bash
ctest --preset windows-release-managed-tests
```

或：

```bash
.\test.ps1 -Managed
```

解释规则：

- 这条入口运行完整受管 CTest 目标集合。
- 当前最新正式 rerun 已 `104/104 PASS`。
- 它不等价于 Linux 当前 `104/104` managed 结果。
- `T041` 已修掉 Windows 目录句柄权限导致的
  `FlushFileBuffers ... GetLastError=5` 主信号；如果旧日志里仍看到这个错误，
  应按“修复前的 Windows durability 语义问题”理解，而不是继续当作长路径
  harness 问题。
- 当前已无剩余 Windows full managed 失败项；完整状态统一见
  [windows-full-managed-failure-matrix.md](./windows-full-managed-failure-matrix.md)。

## 9. 哪些测试属于 Windows fallback，哪些不属于

Windows fallback 当前只跑保守 baseline：

- `CommandTest`
- `KvStateMachineTest`
- `TimerSchedulerTest`
- `ThreadPoolTest`

默认不进入 Windows platform-neutral fallback 的受管测试包括：

- `RaftKvServiceTest`
- `RaftElectionTest`
- `RaftLogReplicationTest`
- `RaftCommitApplyTest`
- `RaftSplitBrainTest`
- `RaftLeaderSwitchOrderingTest`
- `PersistenceTest`
- `RaftSnapshotRecoveryTest`
- `RaftSnapshotDiagnosisTest`
- `RaftSnapshotCatchupTest`
- `RaftSnapshotRestartTest`
- `RaftSegmentStorageTest`
- `SnapshotStorageReliabilityTest`
- `RaftReplicatorBehaviorTest`
- `RaftIntegrationTest`

更完整的测试角色边界见 [tests/README.md](../../tests/README.md)。

## 10. Linux-specific 测试组说明

以下内容必须视为 Linux-primary 或 Linux-specific 证据，而不是跨平台已完成能力：

- 当前时序敏感的 flaky 验收路径
- 未来的精确 `fsync`、directory sync、rename/replace、prune/remove、
  partial write failure injection
- 任何依赖 Bash-first 测试编排和 `--keep-data` 现场保留的流程

因此：

- 文档、计划和任务中提到的 Linux-specific 分组，必须显式标注。
- 这些分组在 Windows/macOS 上不能被默认为“等价完成”。

## 11. Windows/macOS fallback 与 deferred 说明

当前阶段对 Windows/macOS 的要求是：

- Windows 至少能使用 preset-based fallback：

```bash
.\test.ps1 -All
```

- 其他非 Bash 环境至少能理解并使用平台无关 fallback：

```bash
ctest --preset debug-tests --output-on-failure
```

- 需要明确知道，Linux-specific durability / failure-injection / crash-style
  证据尚未在 Windows/macOS 上完成 runtime 验证。
- 若后续需要更明确的非 Bash 入口，应由后续任务补充 `test.ps1` 或等价说明；
  当前阶段仍属于 planned fallback / deferred scope。

## 12. manual-only / diagnostic-only 程序说明

不是所有 `tests/` 下的文件都会进入受管 GTest / CTest 回归。

- `persistence_more_test.cpp` 保留为 manual-only / diagnostic-only 的两阶段恢复演示程序
- 它适合人工查看 marker、manifest 和 retained artifacts
- 它不属于 Linux 主回归入口，也不属于 Windows platform-neutral fallback

更完整的测试角色说明见 [tests/README.md](../../tests/README.md)。

## 13. 当前实现顺序

1. 冻结已完成能力和剩余风险边界
2. 收敛 Linux flaky 验证路径
3. 增加 exact durability failure injection
4. 补齐 restart trusted-state 覆盖
5. 强化 catch-up、leader switch、apply/replay consistency
6. 收敛 CTest、Bash 和后续 PowerShell 入口
7. 完成失败定位与平台支持文档
