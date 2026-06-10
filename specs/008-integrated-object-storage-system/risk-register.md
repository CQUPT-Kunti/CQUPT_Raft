# Risk Register: Integrated Object Storage System

**Feature**: 008-integrated-object-storage-system  
**Date**: 2026-06-10

## R-001: ViewNode 被误实现为 Raft membership authority

**Risk**: 开发时为了方便把 ViewNode 注册结果直接用于 quorum 或 voter 列表。  
**Impact**: split-brain、非法 commit、membership 与 Raft log 不一致。  
**Mitigation**: 合同和任务明确 ViewNode 只观测；quorum 测试覆盖“注册不等于 voter”。  
**Owner Area**: `modules/view`, `modules/raft/node`, `tests/integrated_object_storage_quorum_test.cpp`

## R-002: 真实 payload 意外进入 Raft

**Risk**: CreateObject/CommitObject 或 task report 中携带 chunk bytes。  
**Impact**: Raft log 膨胀、snapshot 膨胀、恢复慢、违反阶段目标。  
**Mitigation**: metadata contract 禁止 payload；增加 payload boundary audit 测试。  
**Owner Area**: `proto/metadata.proto`, `modules/raft/metadata`, `modules/raft/common`, `tests/integrated_object_storage_e2e_test.cpp`

## R-003: 大文件路径仍整文件入内存

**Risk**: upload coordinator 或 client 为计算 etag 拼接完整文件。  
**Impact**: 大文件 OOM，性能目标失败。  
**Mitigation**: 基础任务先改为 streaming checksum 或调用方提供对象 checksum；测试覆盖内存边界。  
**Owner Area**: `modules/store/upload`, `apps/storage_client.cpp`

## R-004: node.identity durability 跨平台弱化

**Risk**: Linux 使用真实 fsync，而 Windows 分支 no-op success。  
**Impact**: 重启身份漂移或损坏后难以诊断。  
**Mitigation**: identity 写入 contract 明确平台语义；unsupported 返回明确错误；跨平台 durability 测试。  
**Owner Area**: `modules/store/common`, `modules/store/io`, planned identity helper

## R-005: StorageNode dynamic registration 与 placement facts 不一致

**Risk**: ViewNode 观测状态和 PlacementManager 使用的健康/容量快照不一致。  
**Impact**: 写计划选择 dead/full 节点，上传失败或 orphan chunk 增多。  
**Mitigation**: placement 只消费 live、fresh、capacity-valid snapshot；记录 decision_reasons。  
**Owner Area**: `modules/view`, `modules/store/placement`

## R-006: 协议变更范围扩大

**Risk**: 为了 ViewNode 或 streaming RPC 改动既有 metadata/storage RPC 语义。  
**Impact**: 破坏现有测试和调用方，扩大迁移范围。  
**Mitigation**: ViewNode 使用 additive contract；第一阶段使用 bounded chunk RPC；任何 proto 变更必须有 contract test。  
**Owner Area**: `proto/`, `modules/raft/service`, `modules/store/node`

## R-007: 测试启动真实多进程导致不稳定

**Risk**: E2E 测试依赖端口、时间、进程清理，容易 flake。  
**Impact**: CI 和本地验收不可靠。  
**Mitigation**: 测试 helper 管理端口和 data_dir；超时和日志路径可诊断；低并发运行完整组。  
**Owner Area**: `tests/support/integrated_cluster_test_utils.h`, `tests/CMakeLists.txt`

## 平台验收状态标记

- `Linux 已验证`：已有 Linux 构建目录下的可执行测试或定向 smoke 结果支撑。
- `Linux-only 计划验证`：第一阶段要求在 Linux 完成，但当前仍是 disabled/scaffold、尚未收口到最终验收矩阵。
- `Windows fallback/smoke`：当前只承诺配置、路径、启动、最小 durability contract 或最小命令层面的 smoke 期望，不等价于 Linux 故障矩阵 PASS。
- `Windows 待实机验证`：尚未有 Windows 实机验证证据，不能写成已通过。

## R-008: US6 Linux-primary 故障验证被误写成跨平台已通过

**Risk**: 把 Linux-first 的 recovery/concurrency/failure 结果直接当成 Windows 或“全平台”已验收。  
**Impact**: 版本验收结论失真，Windows 上的 restart、durability、并发和路径问题会被过早掩盖。  
**Mitigation**: 风险登记和后续 validation matrix 必须显式区分 Linux 已验证、Linux-only 计划验证、Windows fallback/smoke、Windows 待实机验证。  
**Current Status**:
- `Linux 已验证`：
  - T076 `StorageNode restart after committed upload` 已有可执行验证，覆盖同一 `data_dir` 重启后按 committed manifest 读取 chunk、逐 chunk checksum 和最终 SHA-256。
  - T077 已验证“未提交对象不可见 + live chunk 留作 cleanup scaffold 前置条件”。
  - T079 已验证“无健康/容量足够 StorageNode 时 upload 明确失败且对象不可见”。
  - T084 已把 `integrated_object_storage_recovery` / `integrated_object_storage_concurrency` target 与 labels 接入。
- `Linux-only 计划验证`：
  - T077 disabled cleanup hook 用例仍未成为最终 recovery acceptance。
  - T078 的 `DISABLED_T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256` 仍未启用，因此 100-op 真实 round-trip 压测还不能写成已完成验收。
- `Windows fallback/smoke`：
  - 只能声明配置解析、路径兼容、启动入口和最小构建/运行路径为支持目标。
- `Windows 待实机验证`：
  - restart、failure、并发、容量失败和大文件 bounded transfer 还没有 Windows 实机证据。
**Owner Area**: `tests/CMakeLists.txt`, `tests/integrated_object_storage_recovery_test.cpp`, `tests/integrated_object_storage_concurrency_test.cpp`, `specs/008-integrated-object-storage-system/`
**Follow-up**: T086+ 的 validation 收口任务；T088 的 Windows startup/path smoke notes。

## R-009: Windows durable publish / rename / flush 语义与 Linux 不完全等价

**Risk**: Linux 的 `fsync + rename + directory sync` 语义如果在 Windows 上被简化成“看似成功”的弱操作，会导致 chunk publish、identity publish 或恢复边界不一致。  
**Impact**: 重启后可能出现 chunk 已写但未可靠 publish、identity 文件漂移、manifest 与 data-plane 可读性不一致。  
**Mitigation**:
- required durability operation 在 Windows 上必须使用真实等价路径，例如 `FlushFileBuffers`、`MoveFileExW` 或等价安全发布序列。
- 如果缺少等价保证，必须返回明确错误或在 durability contract 中明确较弱语义；禁止 silent no-op success。
- Windows fallback 不得通过绕过 publish/recovery contract 来“伪造通过”。  
**Current Status**:
- `Linux 已验证`：节点 identity durability contract、StorageNode restart/recovery 方向已有 Linux 侧测试与任务约束。
- `Windows fallback/smoke`：设计上要求等价 durability contract，但当前 risk register 只能把它记为支持目标，不记为已验收。
- `Windows 待实机验证`：需要后续实机验证 `FlushFileBuffers`、重命名发布、临时文件替换和恢复语义。  
**Owner Area**: `modules/cluster/node_identity.cpp`, `modules/store/chunk/`, `modules/store/io/`, `tests/node_identity_test.cpp`, `tests/integrated_object_storage_recovery_test.cpp`
**Follow-up**: Windows durability smoke / follow-up validation；T086+ 收口任务。

## R-010: Windows 路径、临时目录、文件锁和启动差异影响 US6 验收

**Risk**: Linux 下可行的 `/tmp`、路径分隔符、文件锁、端口释放时序和脚本调用假设，在 Windows 上可能表现不同。  
**Impact**: startup smoke、recovery 测试、并发测试或下载临时文件 publish 路径出现平台特有失败。  
**Mitigation**:
- 路径与临时目录使用 `std::filesystem` 和平台默认 temp 目录，不把 Linux-only 路径写进生产或测试 contract。
- 下载临时文件 publish 需要接受 Windows 上 rename/replace 语义差异，不能把失败吞掉。
- `flock` 仅是 Linux 开发并发约束；Windows 需要等价的“单 build 目录单写者”串行策略，不能假设 `flock` 可用。
- app 启动、端口绑定和 data_dir 复用只能声明为 Windows smoke/support 目标，直到有实机证据。  
**Current Status**:
- `Linux 已验证`：quickstart 与 T084 相关 build/test 命令以 Linux 构建目录为主。
- `Windows fallback/smoke`：`quickstart.md` 已有 Windows 命令示例和路径说明，但 US6 故障矩阵不能因此视为已通过。
- `Windows 待实机验证`：进程启动、端口复用、文件锁、临时目录清理和 rename 失败诊断仍需实机验证。  
**Owner Area**: `apps/*.cpp`, `tests/support/integrated_cluster_test_utils.h`, `specs/008-integrated-object-storage-system/quickstart.md`
**Follow-up**: T088 Windows startup/path smoke notes；后续 Windows 实机验证任务。

## R-011: 100-op 并发与大文件 bounded transfer 容易被过早宣称为已验收

**Risk**: 把测试计划、disabled stress skeleton 或 bounded concurrency 设计边界误写成“已完成 100 operations 压测和大文件全平台验收”。  
**Impact**: 吞吐、内存上界、资源占用和故障传播的真实风险被低估。  
**Mitigation**:
- 必须区分“有界资源策略已实现”与“高压并发已完成端到端验收”。
- 大文件路径只能声明为 chunked/bounded 设计与 Linux-primary 验收目标，不能因为小型 fixture 或 plan test 通过就宣称全部完成。
- Windows 上的大文件/并发只允许记为 fallback 或待验证。  
**Current Status**:
- `Linux 已验证`：
  - T083 已在 `object_transfer` 中显式收紧 session 级 bounded concurrency 预算。
  - T078 已验证 bounded resource 计划、SHA-256 验收规则和 failure classification 约束。
- `Linux-only 计划验证`：
  - 真正的 `DISABLED_T078ConcurrentUploadDownloadHundredOperationsRoundTripWithFinalSha256` 仍未启用。
  - 大文件 bounded transfer 的最终压测证据尚未在本风险登记中记为完成。
- `Windows 待实机验证`：
  - 尚无 Windows 下 100-op 并发或大文件 bounded transfer 的实机结果。  
**Owner Area**: `modules/store/transfer/object_transfer.cpp`, `tests/integrated_object_storage_concurrency_test.cpp`, `apps/storage_client.cpp`
**Follow-up**: T086+ validation 收口；后续真正 100-op round-trip 验收。

## R-012: payload boundary 在故障恢复和跨平台 fallback 中被意外放松

**Risk**: 为了简化 Windows fallback、故障调试或恢复验证，把真实 payload 写入 Raft log、Raft snapshot、metadata snapshot 或风险文档/诊断。  
**Impact**: 违反阶段目标，扩大持久化体积，并让 Linux/Windows 路径都偏离 metadata/control-plane 与 data-plane 边界。  
**Mitigation**:
- payload boundary 是跨平台硬约束，不因 Linux-only 测试、Windows fallback 或文档任务而放松。
- 任何平台的 recovery/concurrency/durability fallback 都只能记录 metadata、checksum、size、chunk_id、node_id、version 等事实。
- 风险登记和任务报告不得记录真实 payload。  
**Current Status**:
- `Linux 已验证`：T022 payload boundary audit 已作为基础验证存在；T076 报告已明确 metadata command / metadata snapshot 不包含真实 payload。
- `Windows fallback/smoke`：只允许保持同一 boundary，不允许引入“Windows 特殊 inline payload”捷径。
- `Windows 待实机验证`：仍需在后续 Windows smoke/validation 中确认相关诊断和恢复路径没有越界。  
**Owner Area**: `proto/metadata.proto`, `modules/raft/metadata`, `modules/store/transfer/`, `tests/integrated_object_storage_e2e_test.cpp`
**Follow-up**: 后续 validation matrix 与 Windows smoke 验证。
