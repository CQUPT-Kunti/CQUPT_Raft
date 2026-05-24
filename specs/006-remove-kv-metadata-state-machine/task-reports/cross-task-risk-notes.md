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

## T055 追加注意：no-KV 审计与脚本入口存在覆盖盲区

- 来源任务：T055
- 注意点：
  - 当前 [test.sh](/home/yangjilei/Code/C++/CQUPT_Raft/test.sh:1) 已是 `T051` Linux 全量验证脚本，不再提供 `--group` 分发；`./test.sh --group no-kv` 实际不会成为轻量 no-KV 审计入口。
  - 当前 [test.ps1](/home/yangjilei/Code/C++/CQUPT_Raft/test.ps1:37) 与 [CMakePresets.json](/home/yangjilei/Code/C++/CQUPT_Raft/CMakePresets.json:89) 仍保留 `CommandTest / KvStateMachineTest / TimerSchedulerTest / ThreadPoolTest` 的 Windows conservative fallback 子集表述。
  - 当前 [tests/no_kv_surface_audit.cmake](/home/yangjilei/Code/C++/CQUPT_Raft/tests/no_kv_surface_audit.cmake:110) 仅把 `CommandType::kSet/kDelete`、`KvStateMachine`、`test_state_machine.cpp`、`SetCommand/DeleteCommand` helper 记为 tolerated blocker，并未提升为 strict fail；同时未覆盖 `CMakePresets.json`。
- 可能影响：
  - `T058` 若只强化 `tests/no_kv_surface_audit.cmake` 而不扩展到 preset / script 文案与入口，可能出现“审计 PASS，但 Windows fallback 和脚本入口仍宣传 KV residual”的假阴性。
  - `T059` 若只修 `test.sh --group no-kv` 的命令分发，而不同时收口 `test.ps1` / `CMakePresets.json` 的 fallback 子集，最终 no-KV 主验证入口仍不一致。
- 建议处理：
  - `T058` 需要把 `CMakePresets.json`、`test.ps1` 中 `KvStateMachineTest` fallback 文案与 target/filter 一并纳入审计范围。
  - `T059` 需要恢复或重建 `test.sh` 的分组分发能力，并显式把 `no-kv` 组收敛为 direct `NoKvSurfaceAudit` 与必要 metadata-only smoke。
- 当前是否阻塞：
  - 不阻塞 `T055` 静态审计结论，但会直接影响 `T058/T059` 的设计边界与最终收口可信度。

## T056 追加注意：生产 KV 符号删除后旧测试编译链路待 T057 收口

- 来源任务：T056
- 注意点：
  - 本轮已从生产代码中删除 `CommandType::kSet/kDelete`、`KvStateMachine`、`state_machine.h/.cpp`、`DebugGetValue()`、`KvRequestLimits/kv_limits` 等 KV 残留。
  - 当前 `tests/` 仍保留大量对这些旧生产符号的直接引用，包括：
    - `tests/test_command.cpp`
    - `tests/test_state_machine.cpp`
    - `tests/snapshot_test.cpp`
    - `tests/persistence_test.cpp`
    - `tests/test_raft_snapshot_recovery.cpp`
    - `tests/test_raft_snapshot_restart.cpp`
    - `tests/test_raft_snapshot_catchup.cpp`
    - `tests/test_raft_election.cpp`
    - `tests/test_raft_commit_apply.cpp`
    - `tests/raft_integration_test.cpp`
    - `tests/metadata_state_machine_test.cpp`
- 可能影响：
  - 生产主路径 `raft_demo` / `raft_metadata_client` / `NoKvSurfaceAudit` 已可构建并执行，但旧测试目标在重新构建时，预期会因为缺失 `kSet/kDelete`、`DebugGetValue()`、`state_machine.h` 等符号而失败。
- 建议处理：
  - `T057` 需要统一迁移或退役上述测试与 helper，不能在多个测试文件里零散兼容旧符号。
  - `T058` 需要在 `T057` 完成后把 `tests/test_state_machine.cpp`、`SetCommand/DeleteCommand` helper 等 tolerated blocker 升格为 strict fail。
- 当前是否阻塞：
  - 不阻塞 `T056` 生产代码清理完成，但会直接影响后续测试全量构建，必须由 `T057` 收口。
