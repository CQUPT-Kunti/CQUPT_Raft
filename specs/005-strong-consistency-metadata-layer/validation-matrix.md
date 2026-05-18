# Validation Matrix: Strong Consistency Metadata Layer

## Scope

当前文件只规划验证矩阵，不读取或分析现有 `tests/**`，不运行测试，不修改源码。后续 tasks 阶段应把这些条目落到具体测试文件、命令和 CI 分组中。

## Matrix

| ID | Scenario | Priority | Setup | Action | Expected Result | Evidence |
|----|----------|----------|-------|--------|-----------------|----------|
| VM-001 | Create 后 Pending 不可见 | P1 | leader 可用，object_key 不存在 | CreateMetadataRecord 后立即 Head/List | create 成功，状态 Pending；Head/List 均不返回 object | 客户端输出 code/state/head/list |
| VM-002 | Commit 后 Committed 可见 | P1 | 已有 Pending record | CommitMetadataRecord 后 Head/List | commit 成功，状态 Committed；Head 返回完整 MetadataRecord；List 包含 object_key | 客户端输出 record 和 log_index |
| VM-003 | Duplicate create 幂等 | P1 | create 请求已成功 | 同一 request_id、同一内容再次 create | 返回 IDEMPOTENT_REPLAY 或等价成功；无重复记录 | Head/List 仍符合 Pending 不可见 |
| VM-004 | request_id 内容冲突 | P1 | request_id 已用于 create | 同一 request_id 携带不同 object_key 或 manifest | 返回 IDEMPOTENCY_CONFLICT | 错误码和诊断 message |
| VM-005 | Commit retry 幂等 | P1 | Pending record 存在 | 同一 commit request_id 重复提交 | 只有一个 Committed 结果；Head/List 稳定可见 | commit response + Head/List |
| VM-006 | Missing Pending commit | P1 | object_key never-created | CommitMetadataRecord | 返回 NOT_FOUND 或 STATE_CONFLICT，不创建可见记录 | Head/List not found |
| VM-007 | Delete tombstone | P2 | Committed record 存在 | DeleteMetadataRecord 后 Head/List | 状态 Deleted；Head/List 不返回 object；tombstone 内部可恢复 | delete response + Head/List |
| VM-008 | Delete retry 幂等 | P2 | 删除已成功 | 同一 delete request_id 再次 delete | 返回幂等结果；状态仍 Deleted；无复活 | delete response + Head/List |
| VM-009 | Delete Pending conflict | P2 | Pending record 存在 | DeleteMetadataRecord | 返回 STATE_CONFLICT；Pending 仍不可见 | code/state + Head/List |
| VM-010 | Deleted 防旧请求复活 | P2 | object 已 Deleted | 重放旧 create/commit request | 不得变为 Committed；Head/List 仍 not found | Head/List + conflict code |
| VM-011 | Snapshot/restart 恢复 committed metadata | P2 | 多个 Committed records 已存在 | 触发 snapshot/restart 后 Head/List | committed records 全部恢复可见 | 重启后客户端 Head/List |
| VM-012 | Snapshot/restart 恢复 tombstone | P2 | Deleted tombstone 已存在 | 触发 snapshot/restart 后 Head/List | deleted object 仍不可见；旧请求不能复活 | 重启后 Head/List + retry |
| VM-013 | Pending restart 不外部可见 | P2 | Pending record 存在 | restart 后 Head/List | Pending 不可见；若保留内部 Pending，也必须等待 commit 才可见 | 重启后 Head/List |
| VM-014 | Leader failover 保留 committed metadata | P3 | Committed records 存在 | leader failover 后向新 leader Head/List | committed metadata 不丢失 | 新 leader 客户端输出 |
| VM-015 | Leader failover 不暴露 Pending | P3 | Pending record 存在 | leader failover 后 Head/List | Pending 不可见 | 新 leader Head/List |
| VM-016 | Failover 后 commit retry | P3 | commit 请求结果不确定 | 新 leader 上用相同 request_id 重试 | 返回已提交结果或完成提交；无重复记录 | commit response + Head/List |
| VM-017 | Simulated manifest validation | P3 | 客户端生成 manifest | create with object_size/chunk_size/chunk_count/checksum/mock_locations | 合法 manifest 被接受；非法 manifest 返回 INVALID_ARGUMENT | create response |
| VM-018 | Payload boundary | P3 | payload 超过规划上限 | CreateMetadataRecord | 返回 INVALID_ARGUMENT；不写入 Pending | response + Head/List |
| VM-019 | List deterministic ordering | P3 | 多个 Committed records | ListMetadataRecords | 只返回 Committed，按 object_key 确定性排序 | list output |
| VM-020 | StorageNode boundary | P4 | mock_locations 指向不存在节点 | create/commit/head/list | metadata 操作不要求真实 StorageNode 或 chunk 文件存在 | 成功响应 + 无文件依赖 |

## Validation Layers

### Unit-Level Planning

- Metadata command codec 验证 create/commit/delete 编解码、fingerprint 和 invalid argument。
- State machine 验证状态转换、committed-only visibility、tombstone、幂等表。
- Snapshot model 验证 committed metadata、tombstone 和必要幂等表恢复。

### Service-Level Planning

- Metadata write API 验证 leader hint、term、log_index、request_id 和细分错误码。
- Metadata read API 验证 Head/List 不暴露 Pending 或 Deleted。
- Not-leader path 验证客户端可以复用 request_id 重试。

### Client Scenario Planning

- create -> head/list not found。
- create -> commit -> head/list found。
- create/commit/delete retry。
- delete -> head/list not found。
- failover/restart 后读后写验证。

## Platform Notes

- 平台中立验证应覆盖状态机纯内存语义和 API 语义。
- Linux 可作为 restart/failover 演示的主验证平台。
- 如果后续实现触碰 durability 或 snapshot 文件发布，必须明确 Windows/macOS 的等价行为、错误返回或 deferred follow-up。

## Out Of Scope Validation

- 不验证真实文件上传下载。
- 不验证真实 chunk 落盘。
- 不验证 StorageNode 可达性。
- 不验证 chunk replication、纠删码、rebalance、S3 协议。
- 不通过读取 Raft 内部日志或禁止路径作为验收手段。
