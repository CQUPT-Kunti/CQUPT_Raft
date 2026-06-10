# 008 阶段验收矩阵

本矩阵用于收口 `spec.md` 的 success criteria、`tasks.md` 的任务拆分、`quickstart.md` 的 CLI/启动流程，以及 `risk-register.md` 中已经明确记录的 Linux / Windows 验证边界。

## 状态词汇

| 状态 | 含义 |
|---|---|
| `passed` | 已有明确的 Linux 定向测试、CLI smoke 或任务报告证据支持，不把未运行内容写成通过 |
| `implemented` | 代码/文档/测试入口已落地，但本矩阵对应的验收链路还没有完整通过证据 |
| `scaffold` | 只完成了验收骨架、前置条件测试或局部约束，还不是完整 acceptance |
| `disabled` | 存在明确的 disabled 用例，占位后续启用条件 |
| `pending verification` | 需要后续补跑、补 smoke 或补实机验证后才能下结论 |
| `not yet run` | 当前没有执行记录，尤其用于 Windows 实机验证边界 |

## 验收矩阵

### US1 / US2：上传下载、边界约束与配置启动

| 验收场景 | 对应 US / Tasks | 主要验证入口 | Linux 状态 | Windows 状态 | 当前状态 | 关键风险 / 失败诊断 | Follow-up |
|---|---|---|---|---|---|---|---|
| 真实文件 upload/download，最终 SHA-256 一致；覆盖 64 MiB 或等价真实文件路径 | US1, T026, T037, T039, T049 | `storage_client upload/download` 手工 smoke；`integrated_object_storage_e2e` happy-path round-trip scaffold | 已有命令形态和 E2E scaffold，但完整真实 round-trip acceptance 仍未在任务报告中收口为 PASS | 仅有命令示例和 fallback/smoke 预期；无实机证据 | `scaffold + pending verification` | 不能把 quickstart 命令示例误写成真实多进程上传下载已验收 | T090-T092 |
| checksum mismatch download fail-fast，损坏文件不得发布 | US1, T028 | `IntegratedObjectStorageE2ETest.ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture`；disabled 完整 fail-fast 用例 | 前置条件和失败契约已 PASS；完整“损坏文件不发布”仍是 disabled 用例 | 无 Windows 实机结果 | `scaffold + disabled + pending verification` | 当前不能把 precondition scaffold 当成完整下载链路通过 | 后续启用 T028 disabled 用例；T090-T092 |
| payload 不进入 Raft log / Raft snapshot / metadata snapshot | US1, T022, T025 | `IntegratedObjectStorageE2ETest.PayloadBoundaryAudit*`；upload bounded checksum tests | 已有明确 Linux 侧 PASS 证据；风险登记 R-012 已锁定为硬约束 | 仅允许 fallback/smoke 边界说明，仍待实机确认诊断路径不越界 | `passed` | 任何平台都不能为 fallback 或调试把真实 payload 写进 metadata / Raft | Windows 后续 smoke；T090-T092 |
| `PENDING` hidden / `COMMITTED` visible | US1, T027 | `IntegratedObjectStorageE2ETest.ManifestVisibilityPendingHiddenCommittedVisible` | PASS；已验证 `Head/List` 在 `PENDING` 时不可见、`COMMITTED` 后可见 | 无独立 Windows 实机记录 | `passed` | 不允许用 ViewNode 观测信息或 StorageNode 本地状态推导对象可见性 | Windows 后续 smoke；T090-T092 |
| cluster config generation 覆盖 1/3/5/7 MetadataNode voter 拓扑 | US2, T040 | `cluster_config_test` 中 T040 相关 generation 用例 | PASS；已覆盖 1/3/5/7 voter 生成与基础校验 | 仅有跨平台路径设计和命令示例；无 Windows 实机结果 | `passed` | 不能把生成成功误写成 app startup 全链路已通过 | T090-T092 |
| per-node config resolution 与 endpoint / data_dir 精确解析 | US2, T042 | `cluster_config_test` 中 `ResolveClusterNodeConfig(...)` 相关用例 | PASS；已验证单节点解析、错误诊断和角色分辨 | 无 Windows 实机结果 | `passed` | 禁止 fallback 到硬编码 demo 节点或默认端口 | T090-T092 |
| 1/3/5/7 voter quorum 计算来自 initial voter membership，而不是 live node 数 | US2, US5, T043 | `cluster_config_quorum_helper_test.computes_majority_quorum_for_1_3_5_7_initial_voters` | PASS；已验证 1/3/5/7 对应 quorum 1/2/3/4 | 无 Windows 实机结果 | `passed` | 这里只验证 quorum helper；不等价于运行时 commit safety 全收口 | T050-T052, T090-T092 |
| `storage_client generate-config` 生成本地集群配置 | US2, T044 | `storage_client generate-config --help`；定向 CLI smoke 生成 JSON | PASS；任务报告已有生成配置和路径处理验证 | 只有 quickstart Windows 命令示例，未见实机 smoke | `passed` | 不应把 Linux 路径假设写死到 JSON 或输出目录 | Windows 实机 CLI smoke；T090-T092 |
| `view_node_app` thin startup / CLI smoke | US2, US4, T045, T072 | `view_node_app --help`；最小启动 smoke | PASS；已验证配置加载、identity 创建/复用、端口绑定和最小生命周期 | `fallback/smoke expectation only`，`not yet run` | `passed` | ViewNode 只是 observation boundary，不是一致性 authority | Windows 实机 smoke；T090-T092 |
| `metadata_node_app` thin startup / identity / raft_id / registration 边界 | US2, US3, US4, T046, T061, T073 | `metadata_node_app --help`；最小启动 smoke；override rejection smoke | PASS；已验证 config 解析、identity/raft_id 校验、最小启动、注册失败不阻止启动 | `fallback/smoke expectation only`，`not yet run` | `passed` | 不能让 `--listen` / `--data_dir` override 造成同一 `raft_id` 漂移 | Windows 实机 smoke；T090-T092 |
| `storage_node_app` thin startup / identity / registration-heartbeat 边界 | US2, US3, US4, T047, T062, T071 | `storage_node_app --help`；最小启动 smoke | PASS；已验证基础启动、identity 装配和最小 help/smoke 路径 | `fallback/smoke expectation only`，`not yet run` | `passed` | 不能把 app startup smoke 等同于完整 data-plane recovery 验收 | Windows 实机 smoke；T090-T092 |

### US3 / US5：ViewNode discovery、观测边界与 quorum safety

| 验收场景 | 对应 US / Tasks | 主要验证入口 | Linux 状态 | Windows 状态 | 当前状态 | 关键风险 / 失败诊断 | Follow-up |
|---|---|---|---|---|---|---|---|
| MetadataNode / StorageNode 注册后可通过 discovery 查询 endpoint 和观测状态 | US3, T058 | `view_node_discovery_test`；`ViewNodeDiscoveryTest` 中 T058 discovery 集成用例 | PASS；真实 gRPC client/service 路径已验证 metadata/storage discovery | 无 Windows 实机结果 | `passed` | discovery 返回的是 observation，不是 membership authority | T090-T092 |
| heartbeat 刷新健康、容量、负载；旧 heartbeat 不覆盖新状态；`LIVE -> STALE -> SUSPECT -> DEAD` | US3, T059 | `view_node_discovery_test`；`ViewNodeDiscoveryTest` 中 T059 heartbeat/liveness 用例 | PASS；已验证 sequence/observed_at 过期心跳被 `kStaleIgnored` 拒绝 | 无 Windows 实机结果 | `passed` | 不允许用 heartbeat 状态降低 Raft quorum | T090-T092 |
| cluster view / leader hint 仅作为 observation，Client 仍需处理 `NOT_LEADER` | US3, T056, T065, T066 | `ViewNodeDiscoveryTest` 观测映射用例；`storage_client status` smoke；`MetadataTransferClient` NOT_LEADER 边界检查 | 观测映射测试 PASS；`status` 已验证无 ViewNode 时明确失败；live cluster-view + leader-hint CLI 验收仍待统一补跑 | `fallback/smoke expectation only`，`not yet run` | `implemented + pending verification` | 不能把 `leader_hint` 当作强一致事实；最多一次有限重试 | T090-T092 |
| ViewNode 注册的 Raft 节点不自动计入 voter | US5, T052 | `integrated_object_storage_quorum` / T052 定向单测 | 已有定向 build 和定向单测通过记录 | 无 Windows 实机结果 | `passed` | 注册是观测存在，不是 membership change | T090-T092 |
| 3 voter 集群死 2 个后不能 commit 新对象；quorum 不随 live node 数降低 | US5, T050 | `integrated_object_storage_quorum` / T050 定向用例 | 用例已实现、build 已过；任务报告因构建锁未留下最终 PASS 证据 | 无 Windows 实机结果 | `implemented + pending verification` | 这是最终 acceptance 的关键安全项，不能用 helper 结果替代 | T090-T092 |
| 5 voter 集群在 2 个节点失效后仍可维持多数提交 | US5, T051 | `integrated_object_storage_quorum` / T051 定向用例 | 用例已实现、build 已过；任务报告因构建锁未留下最终 PASS 证据 | 无 Windows 实机结果 | `implemented + pending verification` | 不能把“实现存在”写成 availability 已通过 | T090-T092 |
| placement 排除 dead / stale 节点；不把非 fresh 观测快照放进新 placement | US3, T060, T063, T064 | `store_placement_manager_test`；T060 定向用例 | PASS；dead/stale exclusion 已验证 | 无 Windows 实机结果 | `passed` | 当前 PASS 主要锁定 dead/stale exclusion；更细的 unhealthy / capacity-invalid 组合诊断仍要结合 T079 一起看 | T079, T090-T092 |

### US4 / US6：identity、restart、cleanup、容量失败与并发

| 验收场景 | 对应 US / Tasks | 主要验证入口 | Linux 状态 | Windows 状态 | 当前状态 | 关键风险 / 失败诊断 | Follow-up |
|---|---|---|---|---|---|---|---|
| `node.identity` first-start 创建稳定身份，restart 复用同一 `node_id`，config mismatch 明确失败 | US4, T067, T068, T069, T071, T072 | `./build/linux/safe/tests/test_node_identity --gtest_brief=1`；`view_node_app` / `storage_node_app` identity smoke | PASS；identity 单测和 app smoke 都已有证据 | Windows durability / path / file-lock 仍待实机验证 | `passed` | 不允许静默覆盖现有 identity，也不允许冲突后重生新身份掩盖问题 | Windows 实机验证；T090-T092 |
| MetadataNode `raft_id` 来自 config generation，并在 app startup 中严格校验 | US4, T070, T073 | `cluster_config_test` T070；`metadata_node_app` 正常启动与 override 拒绝 smoke | PASS；已验证稳定、唯一、和 initial membership 匹配 | `fallback/smoke expectation only`，`not yet run` | `passed` | 不允许把 ViewNode registration 或本地 override 用作 MetadataNode `raft_id` 来源 | Windows 实机验证；T090-T092 |
| durable identity conflict diagnostics：错误 cluster / role / node_id / raft_id / durability 不支持都能清晰失败 | US4, T075 | `test_node_identity` 全量相关用例 | PASS；`12` 个 `NodeIdentityTest` 用例通过，且未放宽 required durability contract | Windows 只定义 contract，不存在实机 PASS 证据 | `passed` | Windows 仍禁止 required durability no-op success | Windows 实机 durability 验证 |
| committed upload 后 StorageNode restart，仍可按 manifest 下载并校验 chunk checksum 与最终 SHA-256 | US6, T076 | `integrated_object_storage_recovery` / T076 用例 | PASS；风险登记 R-008 记为 `Linux 已验证` | `Windows 待实机验证` | `passed` | 不能把同目录 restart 能读出 chunk 误写成全平台 recovery 已通过 | T090-T092 |
| uncommitted / orphan chunk 保持不可见，并形成 cleanup candidate / cleanup hook 接入前置条件 | US6, T077, T080, T081 | `IntegratedObjectStorageRecoveryTest.T077UncommittedWrittenChunkRemainsInvisibleAndLeavesCleanupScaffold`；`storage_garbage_collector` 相关单测 | “不可见 + cleanup scaffold 前置条件”已 PASS；cleanup hook / candidate 已实现并有最小验证，但完整 recovery acceptance 仍未收口 | `Windows 待实机验证` | `implemented + scaffold + pending verification` | 不能把“存在 live chunk 但 metadata 无 committed refs”误写成“已经自动清理完成” | T090-T092 |
| 无健康且容量满足的 StorageNode 时，upload / placement 必须明确失败，且部分对象不可见 | US6, T079 | `integrated_object_storage_recovery.IntegratedObjectStorageRecoveryTest.NoHealthyOrCapacitySufficientStorageFailsUploadAndKeepsObjectInvisible` | PASS；风险登记 R-008 记为 `Linux 已验证` | `Windows 待实机验证` | `passed` | 当前仍可能先创建内部 `PENDING` 记录，但不能让对象可见或可提交 | T090-T092 |
| 100 operations concurrent upload/download，最终只接受完成校验的对象 | US6, T078, T083, T084 | `integrated_object_storage_concurrency.IntegratedObjectStorageConcurrencyTest.T078*`；disabled 100-op round-trip 用例 | active 计划/结果校验用例已 PASS；真正 100-op round-trip 仍是 disabled skeleton，风险登记 R-008/R-011 明确不能写成已验收 | `Windows 待实机验证` | `disabled + pending verification` | 不能把 bounded concurrency 设计或 active scaffold PASS 误写成高压全链路验收 | 启用 T078 disabled 用例；T090-T092 |
| bounded memory / chunked transfer，不把整对象一次性拼接进内存或 metadata | US1, US6, T023, T024, T025, T083 | `storage_upload_coordinator` 相关测试；upload bounded checksum tests；`storage_client` 最小 build | bounded chunk / checksum / session budget 相关验证已 PASS；但“大文件 acceptance”仍未单独收口 | `Windows fallback/smoke expectation only`，`not yet run` | `implemented + pending verification` | 不能把 bounded reader/writer 的局部 PASS 写成 64 MiB 全链路已通过 | T090-T092 |
| transient StorageNode failure retry/backoff | US6, T082 | `storage_transfer_client.cpp` 最小 build；T082 报告中的定向检查 | 实现与最小 build 已完成；未见最终 integrated acceptance PASS 证据 | 无 Windows 实机结果 | `implemented + pending verification` | 当前不能宣称 retry/backoff 已在真实 recovery 场景中完全验收 | T090-T092 |
| orphan / staging cleanup integration hook 不误删已提交 live chunk，且保持 metadata authority 边界 | US6, T080 | `storage_garbage_collector` 相关单测；T080 报告 | 最小实现和相关单测已 PASS；但完整 recovery acceptance 仍待和 T077/T081 合并验证 | 无 Windows 实机结果 | `implemented + pending verification` | StorageNode cleanup 不得自行决定对象是否 `COMMITTED` 可见 | T090-T092 |

### 跨平台验收边界

| 验收场景 | 对应 US / Tasks | 主要验证入口 | Linux 状态 | Windows 状态 | 当前状态 | 关键风险 / 失败诊断 | Follow-up |
|---|---|---|---|---|---|---|---|
| Linux-primary 故障验证与 Windows fallback / pending note 明确分栏，不伪造跨平台通过 | Cross-cutting, T085, T087 | `risk-register.md` R-008 ~ R-012；本矩阵 | 已按风险登记区分 `Linux 已验证` 与 `Linux-only 计划验证` | 已按风险登记区分 `Windows fallback/smoke` 与 `Windows 待实机验证` | `implemented` | 不能把 Linux-only recovery / concurrency / capacity 结果直接写成全平台 PASS | T090-T092 与 Windows 实机验证 |
| Windows startup / config / path / durability smoke 边界 | Cross-cutting, T049, T085, T088 | `quickstart.md` Windows 命令示例；`risk-register.md` R-009/R-010 | Linux 命令路径和启动约束已有记录 | 当前只有命令示例和风险说明；未见 Windows 实机 smoke 报告 | `pending verification + not yet run` | `flock` 仅是 Linux 构建约束；Windows 仍需等价串行化、rename/flush/path 验证 | Windows 实机验证；T090-T092 |
