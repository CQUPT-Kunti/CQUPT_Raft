# T019 MetadataStateMachine request_id 幂等最小逻辑

## 1. 结论

- T019 已完成。
- 本次在 `MetadataStateMachine` 中实现了 `request_id` 最小幂等去重逻辑。
- 成功写命令会记录 `RequestRecord` 和命令 fingerprint。
- 相同 `request_id` 的重复请求不会重复 apply。
- 相同 `request_id` 但不同命令或不同 payload 会返回明确冲突错误。
- 未实现真实 snapshot、MetadataService 接入或默认 wiring 切换。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.h`
  - 新增 `request_fingerprints_`，保存成功请求的 fingerprint
- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 在 `Apply(...)` 中于同一把锁内加入：
    - `request_id` 查重
    - fingerprint 对比
    - replay success / conflict 返回
  - 成功 apply 后为以下命令记录 `RequestRecord + fingerprint`：
    - `CreateBucket`
    - `DeleteBucket`
    - `CreateObject`
    - `CommitObject`
    - `AbortObject`
    - `DeleteObject`
  - 重复 `request_id` + 相同 fingerprint：
    - 返回 `idempotent replay`
    - 不重复修改业务状态
    - 不推进 `last_applied_index_ / last_applied_term_`
  - 重复 `request_id` + 不同 fingerprint：
    - 返回 `idempotency conflict: request_id maps to different command`
- 修改 `tests/metadata_state_machine_test.cpp`
  - 新增空 `request_id` 错误测试
  - 新增 6 类写命令的 replay 去重测试
  - 新增相同 `request_id` + 不同 payload / command_type 冲突测试
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 同步说明 `MetadataStateMachine` 当前已承载 `request_id` 最小幂等去重

## 3. 幂等语义固定

- `request_id` 为空：
  - 返回 `invalid metadata command: missing request_id`
- 只记录成功 apply 的请求
- 失败命令不进入 request_table
- 相同 `request_id` + 相同 payload：
  - 返回 `idempotent replay`
  - 不重复 apply
- 相同 `request_id` + 不同 payload 或不同 `command_type`：
  - 返回 `idempotency conflict: request_id maps to different command`

## 4. apply 边界

- 去重检查与状态变更在同一个 `Apply` 临界区内完成
- replay 请求不会重复修改：
  - `buckets_`
  - `objects_`
  - `object_index_`
  - `chunk_ref_index_`
  - `tombstones_`
- replay 请求不会推进：
  - `last_applied_index_`
  - `last_applied_term_`

## 5. 覆盖范围

- 已覆盖基础幂等测试：
  - `CreateBucket`
  - `DeleteBucket`
  - `CreateObject`
  - `CommitObject`
  - `AbortObject`
  - `DeleteObject`
- 已验证：
  - 重复 `CommitObject` 不重复写 `chunk_ref_index_`
  - 重复 `AbortObject` 不破坏 `tombstones_` / `object_index_`
  - 重复 `DeleteObject` 不破坏 `tombstones_` / `object_index_`

## 6. 边界确认

- `MetadataStateMachine` 未依赖：
  - `KVCommand`
  - `KvStateMachine`
  - `KvService`
  - `raft_kv_client`
- 未修改：
  - `CompositeKvMetadataStateMachine`
  - `RaftNode` 默认状态机装配
- 未删除 KV
- 未进入 T020

## 7. Linux 验证命令

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- `ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

## 8. 为什么只跑这些验证

- 本次只改动 `state_machine` 模块源码、对应测试和模块说明
- 没有修改 proto、service、默认 wiring 或全局构建图
- 最小可证明闭环就是：
  - configure
  - 构建 `test_metadata_state_machine`
  - 运行 `MetadataStateMachineTest` 定向过滤
- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest

## 9. Linux 结果

- Linux configure
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t019-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t019-build.log`
- Linux CTest
  - 结果：`PASS`
  - 统计：`25/25` 通过
  - 日志：`tmp/test-logs/t019-ctest.log`

## 10. 风险与限制

- 当前只实现内存态最小幂等逻辑
- `request_table` 还未进入 snapshot / recovery
- `SaveSnapshot / LoadSnapshot` 仍是占位实现
- 当前只验证了 Linux，Windows 留待后续 Windows 环境补测

## 11. 验收结果

- `request_id` 幂等最小逻辑已实现
- 成功写命令会记录 `RequestRecord`
- 重复 `request_id` 不会重复 apply
- 相同 `request_id + 不同命令/不同 payload` 有明确冲突错误
- 六类写命令均覆盖了基础幂等测试
- 重复 `CommitObject` 不重复写 `chunk_ref_index_`
- 重复 `AbortObject / DeleteObject` 不破坏 `tombstone / object_index`
- `MetadataStateMachine` 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现 `snapshot / service`
- 相关 Linux 构建与个别测试已通过
