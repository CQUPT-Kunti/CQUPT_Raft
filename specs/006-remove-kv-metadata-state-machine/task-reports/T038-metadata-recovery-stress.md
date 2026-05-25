# T038 Metadata Recovery Stress

## 变更摘要
- 新增 `tests/metadata_recovery_stress_test.cpp`，补 3 个 recovery stress 场景：
  - 并发 `CreateObject` / `CommitObject` / `DeleteObject` 与并发 `HeadObject` / `ListObjects` 期间触发 snapshot。
  - 并发写入后整集群 restart recovery。
  - follower 停机落后后，通过 snapshot + tail replay catch-up 恢复一致性。
- 更新 `tests/CMakeLists.txt`，新增 `test_metadata_recovery_stress` target。
- 勾选 `tasks.md` 的 T038。

## 验证的 metadata 一致性事实
- `request_table` 幂等事实：
  - restart 后对已提交 `CreateObject` / `DeleteObject` 重新提案返回 `idempotent replay`。
  - restart 后对同一 `request_id` 发送不同 fingerprint 的命令返回 `idempotency conflict`。
- `tombstone` 删除事实：
  - 并发 `DeleteObject` 后，恢复与 catch-up 后 `HeadObject` 不可见。
  - `FindObject` 保持 `DELETED`，`object_index` / `chunk_ref_index` 不复活。
- `object_table` / `object_index` / `chunk_ref_index` 一致性：
  - `ListObjects` 只暴露 committed object。
  - 可见对象必须同时存在 `object_index` 与 2 条 `ChunkRef`。
  - deleted object 必须从可见列表、`object_index`、`chunk_ref_index` 中消失。
- applied boundary：
  - 校验 `LastAppliedIndex` / `LastAppliedTerm` 在 snapshot、restart、catch-up 后不倒退。
  - follower catch-up 后 `LastAppliedIndex` / `LastAppliedTerm` 与 leader 相等。

## Linux 验证
- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_recovery_stress`
- `ctest --test-dir build/linux --output-on-failure -R "MetadataRecoveryStressTest"`
- 结果：PASS
- 总耗时：约 30s
- 日志：`tmp/test-logs/t038-metadata-recovery-stress.log`

## Windows / KV / CTest 说明
- Windows：未执行。本任务按要求只做 Linux 最小验证。
- KV blocker 迁移：未做；未删除 KV 残留，符合 T038 边界。
- CTest：仅新增 recovery stress target，不扩大到全量回归。

## 未覆盖风险
- catch-up 场景当前验证 follower 与 leader 的 metadata facts 完全一致，但没有额外强制“lagging follower 二次当选 leader 后再做 fingerprint 冲突验证”。
- stress 测试依赖真实调度时序；若 CI 机器极慢，snapshot 触发窗口可能需要放宽超时。
