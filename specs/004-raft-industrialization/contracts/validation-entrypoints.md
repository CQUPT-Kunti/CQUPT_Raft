# Contract: 验证入口约定

## 目标

定义维护者执行工业化验证时应使用的入口约定，并与
[../validation-matrix.md](../validation-matrix.md) 与
[../platform-support.md](../platform-support.md) 保持一致。该约定只说明
验证入口、解释规则和平台边界，不改变 CQUPT_Raft 的协议语义、持久化格式或
公共 API。

## 入口分类

### 1. Linux 主验证入口

- **当前基线**：
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel`
  - `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all`
- **约定**：
  - 这是当前唯一的 Linux 主验收路径。
  - 它与 [../validation-matrix.md](../validation-matrix.md) 中的
    `Linux primary` 定义一致。
  - 它可以承载 Linux-specific 分组、低并发回归和 `--keep-data` 现场保留。
  - 它不能被当作非 Linux 平台唯一入口。
  - Windows PowerShell fallback 不能替代它的 Linux-primary 含义。

### 2. 平台无关 CTest fallback

- **当前基线**：
  - `ctest --preset debug-tests --output-on-failure`
- **约定**：
  - 这是当前跨平台 baseline fallback。
  - 在 Linux 上，当维护者不走 `./test.sh` Bash 主入口、只需要确认
    CTest discover / label contract、或需要更窄的 CTest 复验时，它也是
    Linux 的 CTest fallback。
  - 它应覆盖平台无关的回归子集。
  - 它不能自动替代 Linux-specific crash-style、failure-injection 或
    `--keep-data` 证据。
  - 文档必须明确说明：当某项在
    [../validation-matrix.md](../validation-matrix.md) 中被标注为
    Linux-specific 时，该入口只提供逻辑回归 fallback。

### 3. Windows / 非 Bash preset fallback

- **当前状态**：
  - Windows 已补齐 preset-based fallback：
    - `cmake --preset windows`
    - `cmake --build --preset windows-release`
    - `ctest --preset windows-release-tests`
  - 如需额外验证 Debug 路径：
    - `cmake --build --preset windows-debug`
    - `ctest --preset windows-debug-tests`
- **约定**：
  - 该入口只代表 Windows 的 platform-neutral configure/build/CTest fallback，
    不替代 Linux Bash 主入口。
  - `windows-release-tests` / `windows-debug-tests` 对应保守的 Windows
    platform-neutral baseline 子集，而不是整个 `platform-neutral` 语义桶。
  - 当前实现上，它们使用保守的 test-name 子集
    `^(CommandTest|KvStateMachineTest|TimerSchedulerTest|ThreadPoolTest)\.`，
    用来承载 `platform-neutral-fallback` 的运行入口语义。
  - 它用于 Visual Studio 17 2022 multi-config 生成器，因此通过
    `configuration: Release` 选择配置，而不是依赖 `CMAKE_BUILD_TYPE`。
  - 任何依赖 Bash、`--keep-data`、Linux-specific failure injection、
    directory sync、crash-style 语义或 retained-artifact 诊断的流程，
    都必须继续标注为 Linux-primary 或 deferred，不能因为
    `windows-release-tests` / `windows-debug-tests` 可运行就视为 Windows
    已具备等价运行时证据。

### 4. Windows PowerShell fallback wrapper

- **当前状态**：
  - Windows PowerShell fallback 已提供：
    - `.\test.ps1 -All`
- **约定**：
  - `test.ps1` 是 Windows 用户的推荐一键 fallback 入口。
  - 它默认封装以下 Windows Release platform-neutral 流程：
    - `cmake --preset windows`
    - `cmake --build --preset windows-release`
    - `ctest --preset windows-release-tests`
  - 它不调用 Bash，不执行 Linux-specific 分组，也不声称提供与
    `./test.sh --keep-data` 等价的 retained-artifact 诊断能力。

### 5. Windows full managed CTest sweep

- **当前状态**：
  - Windows full managed CTest 入口已单独提供：
    - `ctest --preset windows-release-managed-tests`
    - `ctest --preset windows-debug-managed-tests`
    - `.\test.ps1 -Managed`
- **约定**：
  - 这些入口与 conservative baseline 分开存在。
  - `windows-release-managed-tests` / `windows-debug-managed-tests` 运行完整受管
    CTest 目标集合，不使用 `CommandTest` / `KvStateMachineTest` /
    `TimerSchedulerTest` / `ThreadPoolTest` 的保守子集过滤。
  - 当前 Windows Release full managed 最新正式 rerun 已 `104/104 PASS`。
  - 该结果不等价于 Linux-specific durability / failure-injection /
    crash-style 语义主解释。
  - 当前完整状态与受管目标矩阵统一见
    [../windows-full-managed-failure-matrix.md](../windows-full-managed-failure-matrix.md)。
  - 即使 full managed 入口存在，也不得把 Linux-specific durability /
    failure-injection / crash-style 语义写成 Windows 已等价验证。

### 6. Windows/macOS wrapper follow-up

- **当前状态**：
  - `test.ps1` 已作为 Windows fallback 落地
- **约定**：
  - T025 已补齐 Windows PowerShell wrapper。
  - 其他非 Bash 环境继续以已有 CMake/CTest 命令作为逻辑回归 fallback。

## 验证顺序约定

推荐顺序必须与 [../validation-matrix.md](../validation-matrix.md) 和
`quickstart.md` 保持一致：

1. 先查看 `validation-matrix.md`，确认当前能力分类、Linux-specific 边界和
   deferred 项。
2. 在 Linux 上使用 `cmake --preset debug-ninja-low-parallel` 与
   `cmake --build --preset debug-ninja-low-parallel` 完成低并发构建。
3. 使用 `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all` 作为 Linux 主回归入口。
4. 如果出现高风险失败，按分组 rerun 命令进入 focused regression。
5. 如果当前环境是 Windows，优先使用 `.\test.ps1 -All` 作为 PowerShell
   fallback；其底层固定调用 `windows`、`windows-release`、
   `windows-release-tests` preset。
6. 如果需要单独观察 Windows 的完整受管 CTest sweep，显式使用
   `ctest --preset windows-release-managed-tests`，或 `.\test.ps1 -Managed`。
7. 如果当前环境不走 Bash 主入口且不适用 Windows preset，则使用
   `ctest --preset debug-tests --output-on-failure` 作为平台无关 fallback。

## 真实分组与 rerun 约定

以下分组直接来自 `test.sh`，文档和任务必须使用这些真实名称：

- `persistence`
- `snapshot-recovery`
- `diagnosis`
- `snapshot-catchup`
- `replicator`
- `replication`
- `election`
- `segment-cluster`

推荐解释：

- `persistence`：重启恢复、hard state、log trusted-state 边界
- `snapshot-recovery`：snapshot/restart 主恢复路径
- `diagnosis`：恢复诊断、skip/fallback、边界错误定位
- `snapshot-catchup`：lagging follower 追赶与 snapshot handoff
- `replicator`：单 follower 复制状态机行为
- `replication`：日志复制与 commit/apply 主路径
- `election`：选举与 split-brain 路径
- `segment-cluster`：高负载 segment/snapshot 组合路径

## CTest label 约定

`tests/CMakeLists.txt` 现在给受管 GTest / CTest 注册以下标签，用于补充
`test.sh` 的 group 解释，而不是替代现有 target 名称或 discover 行为：

- `platform-neutral`
  - 代表跨平台基础回归语义，可进入通用 CTest fallback 解释。
- `durability-boundary`
  - 代表 restart / trusted-state / crash-style / durability boundary 语义。
  - 它要求比普通 platform-neutral baseline 更谨慎的平台范围解释。
- `linux-specific-failure-injection`
  - 代表 executable 中包含 exact `fsync`、directory sync、replace/rename、
    prune/remove、partial write 等 failure-injection 覆盖。
  - 这类标签当前只声明 Linux-specific runtime evidence。
- `linux-primary-diagnosis`
  - 代表 Linux-primary 的 failure localization、retained-artifact 或 stress
    bucket。
  - 这些测试即使可经由 CTest 运行，也不能被简单解释为纯
    platform-neutral baseline。

解释规则：

- `ctest --preset debug-tests --output-on-failure` 仍是通用跨平台 fallback。
- 在 Linux 上，它可以用来确认 `tests/CMakeLists.txt` 的 label 与 discover
  行为仍可工作，但这不改变 Linux-primary / Linux-specific 的验收边界。
- `ctest --preset windows-release-tests` 与 `ctest --preset windows-debug-tests`
  只代表 Windows platform-neutral fallback。
- `ctest --preset windows-release-managed-tests` 与
  `ctest --preset windows-debug-managed-tests` 代表 Windows full managed CTest
  sweep 入口，但不自带“已通过”含义。
- Windows test preset 当前运行的是保守 test-name 子集：
  `CommandTest`、`KvStateMachineTest`、`TimerSchedulerTest`、`ThreadPoolTest`。
- 当命中的测试带有 `linux-specific-failure-injection` 或
  `linux-primary-diagnosis` 标签时，Windows / 非 Bash CTest 入口只提供
  logic fallback，不构成 Linux-specific 验收等价物。
- 若同一 executable 同时带有 `platform-neutral` 与
  `linux-specific-failure-injection` / `durability-boundary`，应解释为
  mixed coverage，而不是纯跨平台 runtime 证明。
- 由于当前过滤粒度是 executable label，而不是单条 test case label，
  `platform-neutral-fallback` 被故意收窄为保守子集；即使某些更大范围的
  `platform-neutral` executable 仍包含跨平台逻辑，它们当前也不计入 Windows
  baseline。
- 当前 Windows preset 没有直接用 label 做最终运行过滤，而是用 test-name
  子集来落实这一保守边界；label 仍然保留为文档化语义分类。

## `test.sh` section map 约定

`test.sh` 的帮助输出和运行期摘要现在必须显式区分以下 section：

### 1. 平台无关基础回归组

- `unit`
- `snapshot-storage`
- `kv-service`
- `segment-basic`
- `election`
- `replication`
- `integration`
- `snapshot-catchup`
- `snapshot-restart`
- `replicator`

解释规则：

- 这些 group 的基础逻辑回归属于 platform-neutral baseline。
- 它们仍可以在 Linux Bash 主入口下低并发重跑，但不能因此被误标记为
  Linux-only 语义。
- 若环境不使用 Bash 主入口，优先退回
  `ctest --preset debug-tests --output-on-failure` 或对应的 `ctest -R`。

### 2. 共享 restart / durability 回归组

- `persistence`

解释规则：

- 该 group 的 restart recovery / trusted-state 主体逻辑属于平台无关回归。
- 若牵涉 exact durability、retained artifacts 或 Bash-first 排障过程，
  解释时必须同时引用 Linux-specific 边界，而不是把整组简单标成
  platform-neutral 或 Linux-only。

### 3. Linux-specific / Linux-primary 聚焦组

- `snapshot-recovery`
- `diagnosis`
- `segment-cluster`

解释规则：

- 这些 group 必须在 `test.sh` 头部或输出中明确标成
  Linux-specific / Linux-primary focus groups。
- 这里的 Linux-specific 指当前主入口解释、时序风险或 retained-artifact
  排障边界，不得被扩写成“协议或业务逻辑只在 Linux 正确”。

### 4. Linux Bash 主扫入口

- `all`

解释规则：

- `all` 必须明确说明执行顺序：
  platform-neutral base regression groups -> `persistence` ->
  Linux-specific / Linux-primary focus groups -> final full-suite check。
- 该入口是 Linux Bash 主验收路径，不等价于非 Bash / 跨平台 fallback。

## Linux-specific 分组与解释规则

下列证据必须显式按 Linux-primary 或 Linux-specific 解释：

- 时序敏感的 flaky 验收路径
- exact durability failure injection 分组，或对应带有
  `linux-specific-failure-injection` 标签的 CTest executable
- 依赖 Bash-first 执行和 `--keep-data` 现场保留的流程

因此：

- 文档中出现这些分组时，必须说明它们不是跨平台 runtime 语义已完成的证明。
- Windows/macOS 当前只能把它们视为 follow-up 或 deferred 项。

## 失败现场保留约定

- Linux 主入口必须支持：
  - `CTEST_PARALLEL_LEVEL=1 ./test.sh --group all --keep-data`
  - `./test.sh --group persistence --keep-data`
  - `./test.sh --group snapshot-recovery --keep-data`
  - `./test.sh --group diagnosis --keep-data`
- 文档必须明确：
  - `--keep-data` 是 Linux Bash 主入口能力
  - `test.sh` 必须把 `--keep-data` 说明放在 section map 或帮助输出中
  - 失败后应先按原 group 使用 `CTEST_PARALLEL_LEVEL=1 ./test.sh --group <name>`
    重跑，再按需追加 `--keep-data`
  - `ctest --preset debug-tests` 不默认提供等价现场保留能力
  - 若需要 retained artifacts 做排障，应回到 Linux 主入口重跑

## 非 Bash / 跨平台 fallback 约定

- 当前文档化 fallback 入口仍是：
  - `ctest --preset debug-tests --output-on-failure`
- Windows 当前额外具备 preset-based fallback：
  - `.\test.ps1 -All`
  - `cmake --preset windows`
  - `cmake --build --preset windows-release`
  - `ctest --preset windows-release-tests`
  - `cmake --build --preset windows-debug`
  - `ctest --preset windows-debug-tests`
- Windows 当前也额外具备 full managed CTest sweep 入口：
  - `ctest --preset windows-release-managed-tests`
  - `ctest --preset windows-debug-managed-tests`
  - `.\test.ps1 -Managed`
- 其中 Windows test preset 默认只覆盖 `platform-neutral-fallback`
  baseline 子集，不运行 Linux-specific failure-injection / diagnosis /
  durability-boundary executable，也不默认包含更大范围的 cluster-style
  `platform-neutral` executable。
- Windows full managed 入口运行的是完整受管 CTest 目标集合，但文档不得把该入口
  的存在写成 Windows 已达到 Linux 当前 `104/104` managed 结果。
- 若需要更细粒度重跑，可使用与 `test.sh` 对应的直接 `ctest --test-dir build/linux -R`
  命令，但解释范围仍属于 platform-neutral logic fallback。
- `test.ps1` 只是 Windows wrapper，不得把它描述成与 Linux Bash 主入口
  完全等价。

## 非目标

- 不新增任何公共验证 API
- 不修改协议或持久化格式
- 不宣称 Windows/macOS 已具备与 Linux 等价的 crash-style runtime 证据
