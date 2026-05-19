# Implementation Plan: Strong Consistency Metadata Layer

**Branch**: `005-strong-consistency-metadata-layer` | **Date**: 2026-05-18 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `specs/005-strong-consistency-metadata-layer/spec.md`

## Summary

本阶段规划将现有 KV demo/API/client 演进为基于 Raft 提交语义的强一致元数据层。核心技术方向是新增或替换上层业务语义为 `StrongConsistencyMetadataStateMachine`，让 Raft 只复制小型元数据命令，不复制真实大文件数据；客户端从 `raft_kv_client` 规划升级为 Metadata Client，用于模拟对象日志、chunk manifest、提交记录、重复请求、读后写验证、删除重试和 leader failover 场景。

本阶段只做规划文档，不修改源码、不实现 StorageNode、不实现真实 chunk 文件存储、不实现大文件真实上传下载、不改 Raft 内核、不读取 `NOTREAD.md` 禁止路径。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: gRPC、Protobuf、CMake、GoogleTest、标准库  
**Current Baseline**: 当前 KV demo 使用 `Command` 表达 `SET|key|value` 和 `DEL|key|`，`KvStateMachine` 维护 key/value map 并通过 snapshot 保存 KV 内容，`KvService` 将 Put/Delete 写请求提交到 Raft，`raft_kv_client` 提供 put/get/delete/status/health/metrics CLI。  
**Targeted Layer**: 上层 demo/API/client 和 state_machine 业务语义，不重新规划 Raft node、replication、storage、snapshot catalog 或 leader election。  
**Storage**: 规划层面要求 metadata snapshot/restart 能恢复 committed metadata 和 tombstone；当前阶段不改持久化格式，后续实现若触碰 snapshot 格式必须单独声明迁移或新格式边界。  
**Testing**: 当前 plan 阶段只输出验证矩阵，不读取或分析现有 `tests/**`。后续 tasks 阶段应规划 unit、service/integration、restart、failover 验证。  
**Target Platform**: Linux 为主要验证环境；Windows/macOS 必须保留等价语义或明确 deferred follow-up。  
**Performance Goals**: 元数据命令保持小对象语义；禁止把真实大文件 bytes 放入 Raft 命令。  
**Constraints**: 不修改 Raft 内核；不修改现有协议语义、持久化格式或公共 API 行为，除非后续实现阶段明确新增 metadata API 并提供兼容计划。  
**Scale/Scope**: 面向 demo/API/client 的元数据语义演进，不交付真实对象存储数据面。

## Constitution Check

*GATE: Passed before Phase 0 research. Re-check after Phase 1 design.*

- Verified existing capabilities affected by this feature are identified and excluded from unnecessary replanning: 当前 Raft election、replication、commit/apply、snapshot、restart recovery 和 KV baseline 均作为受保护能力，不重新分析 004 历史 spec，不重写内核。
- Any protocol, public API, or persisted format change is either absent or explicitly justified with migration and regression coverage: 当前阶段不改源码、不改协议、不改持久化格式；后续若新增 Metadata API 或 snapshot 格式，必须作为新契约变更进入任务拆分。
- Durability, crash-recovery, and restart-recovery implications are stated for every affected path in `state_machine`: metadata snapshot 必须恢复 committed metadata 与 tombstone；Pending 不得因恢复而外部可见。
- Linux-specific validation is explicitly labeled, and Windows/macOS fallback, adaptation, or deferred follow-up is recorded: 验证矩阵区分平台中立语义和 Linux restart/failover 演示；跨平台 durability 保证留到实现任务中显式处理。
- Test entry points are defined at规划层: 当前不读取测试；后续 tasks 应定义 MetadataStateMachine unit、MetadataService API、Metadata Client scenario、restart/failover 验证。
- Observability and diagnostics impact is captured: metadata write response 应保留 leader redirect、term、log_index、request_id、state conflict、idempotency conflict 等可诊断字段规划。

## Project Structure

### Documentation (this feature)

```text
specs/005-strong-consistency-metadata-layer/
├── spec.md
├── plan.md
├── data-model.md
├── api.md
├── client-design.md
├── validation-matrix.md
└── checklists/
```

### Source Code Scope For Future Implementation

```text
modules/raft/common/
  command.h / command.cpp
    规划新增 metadata command 表达，必须避免破坏现有 KV command 语义。

modules/raft/state_machine/
  state_machine.h / state_machine.cpp
    规划新增 StrongConsistencyMetadataStateMachine 或独立 metadata 状态机边界。

modules/raft/service/
  kv_service_impl.h / kv_service_impl.cpp
    规划新增 metadata service 适配层或兼容性迁移层；不放业务状态转换逻辑。

proto/
  raft.proto
    后续如新增 MetadataService，必须作为显式协议扩展，不改变 RaftService 语义。

apps/
  raft_kv_client.cpp
    规划升级或新增 Metadata Client；入口层保持薄，只负责参数解析和请求发起。
```

**Structure Decision**: 当前 plan 只给出未来实现影响范围，不修改这些文件。主模块应是 `modules/raft/state_machine`，客户端入口是 `apps`，RPC 契约在 `proto`，服务适配在 `modules/raft/service`。Raft `node`、`replication`、`storage` 不应成为本 feature 的主动修改对象。

## Phase 0 Research Decisions

### Decision 1: 元数据命令与真实数据面分离

**Decision**: Raft 只复制 metadata command，命令内容包含 object manifest、checksum、mock_locations、request_id 和状态转换意图，不包含真实大文件 bytes 或真实 chunk 文件。  
**Rationale**: Raft 日志适合复制小型、确定性的状态转换；把真实大文件放入共识链路会放大日志、snapshot、catch-up 和恢复成本，也混淆后续 StorageNode/ChunkStore 职责。  
**Alternatives considered**: 直接把文件 payload 放入 `value`；被拒绝，因为违反 feature Non-Goals，并会破坏共识层和数据面的职责边界。

### Decision 2: Pending/Committed/Deleted 三态模型

**Decision**: `MetadataRecordState` 固定为 `Pending`、`Committed`、`Deleted`，只有 `Committed` 对 Head/List 可见。  
**Rationale**: Create 与 Commit 分离可以模拟上传准备、manifest 生成、提交确认；Deleted tombstone 保留删除事实，避免重启或旧请求回放导致对象复活。  
**Alternatives considered**: 只用存在/不存在二态；被拒绝，因为无法表达 Pending 不可见和 tombstone 恢复。

### Decision 3: request_id 幂等表随状态机一起恢复

**Decision**: `ClientRequestId` / `request_id` 的处理结果必须成为状态机可恢复事实，至少覆盖 create、commit、delete 三类写请求。  
**Rationale**: 客户端会在 leader failover、超时、重启后重试；幂等表如果不可恢复，重复写可能产生重复提交、状态倒退或 tombstone 被绕过。  
**Alternatives considered**: 只在客户端本地去重；被拒绝，因为客户端重启、多客户端或 leader 切换后无法保证全局幂等。

### Decision 4: API 采用 metadata 语义命名，不复用 KV 语义表达

**Decision**: 规划独立的 `CreateMetadataRecord`、`CommitMetadataRecord`、`DeleteMetadataRecord`、`HeadMetadataRecord`、`ListMetadataRecords`。  
**Rationale**: KV Put/Get/Delete 无法表达 Pending/Committed/Deleted、manifest、幂等结果和 tombstone；继续复用 KV 命名会掩盖语义差异。  
**Alternatives considered**: 将 metadata JSON 塞入现有 Put/Get value；被拒绝，因为这会把状态转换、幂等和 committed-only visibility 留给客户端约定，无法形成服务端强一致元数据层。

## Phase 1 Design

### StrongConsistencyMetadataStateMachine Boundary

`StrongConsistencyMetadataStateMachine` 是元数据生命周期拥有者，只负责被 Raft 提交后的 metadata command 应用和可见视图维护。

它负责：

- 解析 metadata command 并校验 command type、request_id、object_key 和 manifest 字段。
- 维护 `object_key -> MetadataRecord` 的内部状态视图。
- 维护 `request_id -> IdempotencyEntry` 的幂等结果表。
- 执行 Pending、Committed、Deleted 状态转换。
- 提供 Head/List 的 committed-only 查询语义。
- 在 snapshot/restart 中恢复 committed metadata、tombstone 和必要幂等结果。

它不负责：

- Raft leader election、quorum、term、vote、log matching、replication 或 apply 调度。
- gRPC/protobuf 字段装配和 leader redirect。
- StorageNode 调度、真实 chunk 文件 IO、chunk replication、纠删码、rebalance。
- 大文件上传下载、S3 兼容、权限认证、多租户和配额。

### KVStateMachine / KV Demo 演进路径

1. 保留当前 KV baseline 作为已验证 demo 能力，不在本 plan 阶段改动。
2. 后续实现优先新增 metadata command 与 metadata state machine，而不是把复杂元数据逻辑塞进 `KvStateMachine`。
3. 若需要复用 `IStateMachine` 接口，应保持 `Apply(index, command_data)`、`SaveSnapshot(file_path)`、`LoadSnapshot(file_path)` 的边界不变。
4. `Command` 层需要从 `SET/DEL` 扩展到 metadata operation 时，必须保证现有 `SET|key|value` 和 `DEL|key|` 不被破坏；更安全的路径是新增 metadata command codec，而不是修改 KV command 行为。
5. 服务层只做请求到 command 的转换和响应填充，不能持有 metadata 生命周期状态。
6. 客户端从 KV 参数转向 metadata 子命令和模拟日志生成，但入口层保持薄。

### Metadata Manifest 与未来数据面边界

当前阶段的 metadata manifest 只属于 metadata-plane 契约，Raft 只复制和持久化小型 metadata command，不复制真实大文件 bytes。

- 当前阶段只消费 `object_key`、`object_size`、`chunk_size`、`chunk_count`、`checksum`、`mock_locations`、`payload` 等 metadata-only 字段。
- `payload` 只允许承载小型 metadata-only 附加信息，不能扩展成真实文件内容或 chunk bytes。
- `mock_locations` 当前只是 location reference，用于表达未来 chunk 放置意图或 mock 节点提示；当前阶段不检查真实节点、不检查真实路径、不做本地文件 IO。
- 当前的 Metadata Client create generator 也只生成 mock manifest，不读取真实文件、不生成真实 chunk、不访问 StorageNode 或 ChunkStore。

后续若引入 `StorageNode` / `ChunkStore`，应作为后续 spec / 后续阶段的数据面能力，只能在当前 metadata manifest 边界之上消费以下信息：

- `object_key` 作为对象标识。
- `chunk_size`、`chunk_count` 和后续可细化的 chunk manifest 作为 chunk 布局描述。
- `checksum` 作为对象级或 chunk 级完整性引用。
- `mock_locations` 演进后的 location reference，供数据面映射到真实节点、卷或对象存储位置。

后续数据面接入必须保持以下不变量：

- Raft 仍只复制 metadata command，不复制真实大文件 bytes。
- `HeadMetadataRecord` / `ListMetadataRecords` 仍然只暴露 `Committed` 记录，future data-plane 不得放宽 committed-only visibility。
- `DeleteMetadataRecord` 的 tombstone / `Deleted` 语义保持不变；未来真实 chunk 清理、延迟回收或后台 GC 不得导致对象在 metadata 层重新可见。
- 旧 create/commit 请求不得因后续数据面存在而复活已 tombstoned 的对象。

当前 `005-strong-consistency-metadata-layer` 不扩展以下实现范围：

- `StorageNode` / `ChunkStore` 具体实现。
- 真实 chunk 文件存储、上传、下载或校验。
- chunk replication、repair、rebalance、GC 调度。
- S3 兼容协议、权限认证、多租户或配额控制。

### API Design Summary

详见 [api.md](./api.md)。规划接口必须表达以下结果：

- `OK`: 请求成功并给出 record state 或 visible record。
- `NOT_LEADER`: 当前节点不是 leader，并返回 leader hint。
- `INVALID_ARGUMENT`: object_key、request_id、manifest 字段或 payload 限制不满足。
- `NOT_FOUND`: Head 未找到 committed record；Commit/Delete 找不到可操作对象。
- `IDEMPOTENT_REPLAY`: 同一 request_id 同一意图的重复请求，返回首次逻辑结果。
- `IDEMPOTENCY_CONFLICT`: 同一 request_id 携带不同意图或不同内容。
- `STATE_CONFLICT`: 状态转换非法，例如重复 create 覆盖 committed object，或 delete pending object。
- `INTERNAL_ERROR`: apply、snapshot 或未知内部失败。

### Snapshot / Restart Plan

后续实现必须把以下内容纳入 metadata snapshot：

- 所有 latest state 为 `Committed` 的 `MetadataRecord`。
- 所有 `Deleted` tombstone，包括 object_key、delete_request_id 和删除相关元数据。
- 写请求幂等表中仍需要用于重试判定的 create/commit/delete 结果。
- 可选 Pending 内部状态；如果保存 Pending，恢复后仍必须对 Head/List 不可见。

恢复规则：

- Committed 恢复后立即可通过 Head/List 可见。
- Deleted 恢复后继续隐藏于 Head/List，并阻止旧 create/commit 重放导致对象复活。
- Pending 恢复后只能作为内部非可见状态，除非之后收到有效 CommitMetadataRecord。
- 幂等表恢复后，相同 request_id 的重试必须返回等价结果。

### Leader Failover Plan

本 feature 不修改 Raft failover 机制，只在 metadata 层定义使用方式：

- 写请求只有在 Raft 返回 committed/apply 成功后才向客户端报告可见成功。
- 客户端收到 timeout/not leader 时可以用同一 request_id 重试到新 leader。
- 新 leader 应通过已提交日志或恢复后的 snapshot 获得 committed metadata 和 tombstone。
- 未提交或仅 Pending 的记录不通过 Head/List 暴露。
- Head/List 应面向当前 leader 或具备线性一致读保证的路径；本阶段规划默认由 leader 提供读可见性验证。

## Phase 2 Task Breakdown Guidance

后续 `/speckit-tasks` 应按以下依赖拆分，不在本阶段实现：

1. **数据模型与 command codec**: 定义 `MetadataRecord`、`MetadataRecordState`、`IdempotencyEntry`、metadata command 编解码；保持 KV command 兼容。
2. **状态机核心**: 实现 create/commit/delete/head/list 内部语义、状态转换和幂等结果表。
3. **snapshot/restart**: 定义 metadata snapshot 格式或扩展策略，覆盖 committed metadata、tombstone、必要幂等表恢复。
4. **服务契约**: 新增或规划 `MetadataService` 请求/响应，明确状态码、leader hint、term、log_index、request_id。
5. **服务适配层**: 将 MetadataService 写请求转换为 Raft proposal，读请求执行 committed-only 查询。
6. **Metadata Client**: 支持 create/commit/delete/head/list、模拟 manifest、重复请求、提交重试、删除重试、读后写验证。
7. **验证矩阵落地**: 增加状态机单元验证、服务层验证、客户端场景验证、snapshot/restart 验证、leader failover 验证。
8. **兼容与迁移说明**: 说明 KV demo 与 metadata demo 的共存、替换或命名迁移策略，避免破坏现有使用者。

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| 新增 metadata API 契约会扩大 public API surface | KV API 无法表达 Pending/Committed/Deleted、幂等和 tombstone | 把 metadata JSON 塞入 KV value 不能形成服务端强一致语义，也无法可靠验证 committed-only visibility |
| metadata snapshot 可能需要新格式 | KV snapshot 只保存 key/value，无法恢复 tombstone 与幂等表 | 仅依赖 Raft log replay 会在 snapshot compact 后丢失删除事实或幂等结果 |

## Post-Design Constitution Check

- **Preserve The Verified Core**: 通过。计划不触碰 Raft 内核，只在上层 metadata state machine/service/client 规划演进。
- **Durability Contract Before Convenience**: 通过但标记实现阶段风险。metadata snapshot/restart 必须在实现任务中定义格式、恢复顺序和跨平台 durability contract。
- **Recovery And Consistency First**: 通过。计划明确 committed-only、tombstone、restart、failover 和幂等恢复。
- **Cross-Platform By Default**: 通过。计划不引入平台专属行为；后续 Linux failover/restart 验证需配套跨平台语义验证。
- **Observability And Minimal Surface Change**: 通过。API 规划保留 leader hint、term、log_index、request_id 和细分冲突错误，且不要求重构 Raft 内核。
