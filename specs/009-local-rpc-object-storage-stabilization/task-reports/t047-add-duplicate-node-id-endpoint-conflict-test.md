# T047 Add Duplicate Node Id Endpoint Conflict Test

## Scope

本任务新增 `StorageNodeRegistry` 冲突诊断测试，并在 T046 修复后重新确认整个 `storage_heartbeat_registry` target 全绿。

## Task Source

- `tasks.md`: T047
- `tests/storage_heartbeat_registry_test.cpp`
- `modules/store/node/storage_node_registry.h`
- `modules/store/node/storage_node_registry.cpp`

## Files Changed

- `tests/storage_heartbeat_registry_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t047-add-duplicate-node-id-endpoint-conflict-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## What Changed

在 `tests/storage_heartbeat_registry_test.cpp` 新增：

- `StorageHeartbeatRegistryTest.DuplicateNodeIdOrEndpointConflictDoesNotPolluteTrustedHealthyRecord`

测试覆盖：

- duplicate `node_id` register，但 endpoint 不同
- duplicate `endpoint` register，但 node_id 不同
- duplicate `node_id` heartbeat，但 endpoint 与注册时不一致

并补了正常对照路径：

- endpoint/identity 兼容的 idempotent register 不应被误判为冲突
- 合法更高 sequence heartbeat 仍应被接受

## Conflict Semantics Verified

- duplicate `node_id` + different endpoint：`kConflict`
- duplicate `endpoint` + different `node_id`：`kConflict`
- wrong-endpoint heartbeat：`kConflict`
- 冲突请求不会污染已有 healthy record
- 冲突后仍可继续接受合法更新
- 冲突处理不进入 Raft log、不影响 quorum

## Boundary Checks

- 未把 conflict handling 写成 membership change
- 未放宽 endpoint 冲突
- 未修改 placement / transfer / Metadata / ViewNode peer sync

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

- T047 现在可以重新标记完成。
