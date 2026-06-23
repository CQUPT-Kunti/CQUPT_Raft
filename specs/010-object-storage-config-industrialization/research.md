# Research: Object Storage Config Industrialization

## Decision: Stage 1 keeps the two-pass upload model

**Rationale**: 当前 production transfer 已经通过第一遍 bounded reader 计算 per-chunk checksum 和 object checksum，第二遍上传 payload 并提交 manifest。单遍流式 upload 会同时影响 checksum、write plan、CommitObject 输入和失败恢复，风险超出第一阶段。

**Alternatives considered**:

- 单遍流式 upload：长期正确方向，但会拖慢 first-stage delivery。
- 整文件预读：拒绝，因为违反 bounded memory 和大文件目标。

## Decision: CreateWritePlan is the authoritative logical boundary for selected replicas

**Rationale**: upload 执行层必须只执行 plan，不能在写入循环里按 discovery target 排序选点。第一阶段优先复用 `TransferWritePlan` / `TransferChunkPlan` 表达 per-chunk selected replica nodes，并让 upload 在 plan 缺失时明确失败。

**Alternatives considered**:

- 继续使用 `SortedStorageTargets` fallback：拒绝，因为它按 `node_id` 排序，违反用户约束。
- 立即新增完整 server-side `CreateWritePlan` RPC：暂缓，因为会扩大 proto/service 风险；只有现有 transfer facts 无法承载时才做 additive contract。

## Decision: Remove lexical node-id placement tie-break

**Rationale**: `node_id` 是身份，不是调度权重。资源相同或接近时需要避免固定热点，采用 chunk-scoped deterministic jitter 更符合 per-chunk dynamic placement，并保持测试可复现。

**Alternatives considered**:

- 按 `node_id` 或 endpoint 排序：拒绝。
- 完全随机：拒绝，因为不可稳定测试。
- 保留 config order：拒绝，因为配置顺序不能参与生产优先级。

## Decision: Stage 1 bounded concurrency uses existing session state and minimal helpers

**Rationale**: 当前 `TransferSessionSnapshot` 和内部 session budget 已经能承载方向、进度、chunk size、concurrency 和诊断。第一阶段应扩展这些现有边界，最多新增一个 `.cpp` 内部 helper 聚合 future/task 结果，避免公开 DTO 膨胀。

**Alternatives considered**:

- 新建完整 pipeline/executor 类型族：拒绝，超出 first-stage scope。
- 无界 async/future：拒绝，违反资源保护。

## Decision: Manifest stores actual durable replicas, not planned-only replicas

**Rationale**: CommitObject 之前可能存在慢副本、失败副本或只达到 minimum_successful_writes 的情况。Committed manifest 必须记录实际 durable success replica nodes，否则 read 和 repair 会相信不存在的数据。

**Alternatives considered**:

- 提交 planned nodes：拒绝，会污染 read/repair。
- 提交所有 attempted nodes：拒绝，失败副本不能成为 manifest authority。

## Decision: Read fallback is manifest-scoped

**Rationale**: per-chunk dynamic placement 后，read path 必须以 `chunk_index -> replica_nodes` 为单位从 committed manifest 读取。同 chunk 一个副本失败时，应尝试同 chunk 其他 manifest 副本并校验 checksum。

**Alternatives considered**:

- 从任意 StorageNode discovery 结果猜测 chunk 存在：拒绝，discovery 不是 object visibility authority。
- 固定 replica group 读取：拒绝，store 不是 Raft group。

## Decision: Repair decision B is a design constraint in Stage 1

**Rationale**: 第一阶段不实现完整 repair 闭环，但 placement 和 manifest 必须支持未来 repair 重新选目标节点。缺失副本不强制补回原节点，只要满足 replica_count、minimum_successful_writes、checksum 和 manifest correctness。

**Alternatives considered**:

- 现在实现完整 repair manager manifest coordination：暂缓，依赖 inventory report、source copy、target write、verify、metadata update 和 cleanup。
- 强制补回原节点：拒绝，不符合资源感知和节点故障恢复模型。

## Decision: One production chunk-size code entry

**Rationale**: 用户明确说明 chunk_size_bytes 配置问题已经解决，后续不再从 cluster/config JSON 读取。第一阶段应收敛到单一代码级默认，避免 CLI、config 和 transfer 出现多个硬编码值。

**Alternatives considered**:

- 继续允许 config override：拒绝，违反本 feature 约束。
- 多模块重复 `128MiB` 常量：拒绝，后续维护容易漂移。
