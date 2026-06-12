# T046 Add StorageNode Restart Same Node Id New Incarnation Test

## Scope

本任务收口 StorageNode restart same `node_id` / new `incarnation` 语义，重点验证并修正 `StorageNodeRegistry` 的 register/heartbeat 路径，不修改 Raft membership / quorum。

## Task Source

- `tasks.md`: T046
- `tests/storage_heartbeat_registry_test.cpp`
- `modules/store/node/storage_node_registry.h`
- `modules/store/node/storage_node_registry.cpp`

## Files Changed

- `modules/store/node/storage_node_registry.cpp`
- `tests/storage_heartbeat_registry_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t046-add-storagenode-restart-same-node-id-new-incarnation-test.md`

## What Changed

- 确认了当前 `StorageNodeRegistry::RegisterStorageNode(...)` 的问题：
  - 同一 `node_id` 且 endpoint 相同时，原先会直接走 duplicate/idempotent 返回
  - 在比较 `incarnation_id` 之前就提前退出
  - 这会把“同一长期 `node_id` 的新进程重启注册”误判成普通重复注册
- 做了最小修复：
  - `RegisterStorageNode(...)` 现在在同一 `node_id` 且 endpoint 兼容时，先比较 `incarnation_id`
  - incoming `incarnation_id` 更高时，按同一 StorageNode 的新进程重启处理：
    - 接受注册
    - 更新当前 `incarnation_id`
    - 更新注册 facts
    - 将 `last_sequence` 重置为 `0`
    - 返回 `StorageNodeStatusCode::kOk`
  - incoming `incarnation_id` 更旧时，仍保持拒绝覆盖现有状态
  - endpoint 冲突仍返回 `kConflict`
- 扩展了重启测试，让它覆盖真实 register 路径：
  - 旧进程 heartbeat 把 sequence 推到 `7`
  - 新进程先用同一长期 `node_id` + 新 `incarnation_id` 重新 register
  - 再发送 `sequence=1` heartbeat
  - 断言 register + heartbeat 两个阶段都被正确接受

## Restart Semantics Locked By Test

- 同一长期 `node_id`、endpoint 兼容时，新 `incarnation_id` 不再被当成 duplicate register
- 新 `incarnation_id` 注册成功后，sequence 可以从新的启动基线重新开始
- 同一 `incarnation` 内仍按既有 sequence 规则处理
- 旧 `incarnation` 的迟到 heartbeat 不会覆盖当前新进程状态
- `observed_time` 不会单独越过 `incarnation` / `sequence` 排序

## Boundary Checks

- 未修改 Raft membership / quorum
- 未把 restart 语义放宽成 endpoint 冲突可接受
- 未放宽 duplicate endpoint / duplicate node_id 冲突边界
- 未修改 placement / transfer / ViewNode peer sync

## Validation

- Build:
  - `(
      flock -n 9 || exit 99
      cmake --build --preset debug-ninja-low-parallel --target test_storage_heartbeat_registry
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Test:
  - `(
      flock -n 9 || exit 99
      ctest --preset debug-tests -R "storage_heartbeat_registry|StorageHeartbeatRegistry" --output-on-failure
    ) 9>/tmp/cqupt_raft_build.lock`
  - Result: PASS
- Summary:
  - `storage_heartbeat_registry`: `1/1` PASS

## Build Lock

- Used `flock` build lock: yes
- Lock acquired: yes

## Platform Notes

- Linux: targeted build/test validated.
- Windows: not run, pending.
- macOS: not run, pending.

## Result

PASS

- T046 继续保持完成状态。
- 可以进入后续依赖 StorageNode restart incarnation-aware 语义的任务。
