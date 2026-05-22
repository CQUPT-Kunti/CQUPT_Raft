# Cross-Task Risk Notes

## T031 遗留注意：delete 后同名重建与旧 lifecycle 防护

- 来源任务：T031
- 注意点：
  - T031 中 `CreateObject` 会在同名对象 delete 后重建时清理旧 tombstone。该行为可以支持对象删除后用新 `request_id` 重新创建同名 object，但必须确认不会破坏旧 `request_id` / 旧 lifecycle 的防护语义。
- 可能影响：
  - 如果系统只是简单删除旧 tombstone，而没有依赖 `object_epoch` / `bucket_epoch` / `request_table` / `command_fingerprint` 保留旧 lifecycle 事实，那么旧 create/commit/delete 请求在超时、重试、leader switch 或 restart 后可能错误影响新对象生命周期，造成 stale retry 复活旧对象、覆盖新对象 manifest/chunk_ref，或污染 `object_index`。
- 建议验证：
  - 后续 T035 或 T043 前应补充测试：
  - `CreateObject(c1)` + `CommitObject(m1)`
  - `DeleteObject(d1)`
  - `CreateObject(c2)` + `CommitObject(m2)` 重建同名 object
  - 再重试旧 `request_id c1 / m1 / d1`
  - 断言新对象仍可见，manifest/chunk_ref/checksum 不变，旧请求只能返回 replay/conflict，不能重新 apply，不能污染新 object lifecycle。
- 当前是否阻塞：
  - 不阻塞 T032，但必须在后续并发/恢复验收前被测试锁住。

## T032 追加注意：节点内完成态缓存不覆盖重启后的 admission/replay 语义

- 来源任务：T032
- 注意点：
  - 本轮在 `RaftNode` 内增加了 metadata in-flight / completed proposal 跟踪，用于 bounded admission、同 `request_id` 合流和超时后重试复用。但 completed cache 只驻留内存，不跨节点重启持久化。
- 可能影响：
  - 如果后续任务错误把“节点内 completed cache 命中”当成唯一幂等来源，可能掩盖 restart recovery 后应由 `MetadataStateMachine::request_table` 保证的一致结果，进而在 leader restart、follower catch-up、snapshot replay 边界出现 admission 层和 apply 层语义不一致。
- 建议验证：
  - 在 T035 或 T043 补充以下路径：
  - proposal 首次超时
  - 日志随后提交并 apply
  - leader 或全量集群重启
  - 用同一 `request_id` 重试
  - 断言结果仍由持久化 `request_table` 给出一致 replay/conflict，不依赖节点内存缓存。
- 当前是否阻塞：
  - 不阻塞 T032，但必须在恢复验收阶段验证。
