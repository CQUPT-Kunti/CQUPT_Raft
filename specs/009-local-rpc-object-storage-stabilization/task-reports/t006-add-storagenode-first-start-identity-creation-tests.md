# T006 Add StorageNode First-Start Identity Creation Tests

## Scope

本任务只在 `tests/node_identity_test.cpp` 新增 StorageNode first-start identity lifecycle 测试，并验证 identity_file 首次缺失时应创建本地持久身份。不修改生产实现、proto、CMake、app 文件或其他文档。

## Task Source

- `tasks.md`: T006
- `data-model.md`
- `contracts/identity-lifecycle.md`
- `module-notes.md`
- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`

## Files Changed

- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t006-add-storagenode-first-start-identity-creation-tests.md`

## What Changed

- 新增 `NodeIdentityTest.T006StorageNodeFirstStartMissingIdentityFileCreatesLocalPersistentIdentity`。
- 测试先显式验证首次启动前 `node.identity` 文件不存在时，`LoadNodeIdentity(... require_existing=false)` 返回 `kNotFound`，用于证明“缺失 identity_file 是正常 first-start 输入”，而不是启动错误。
- 随后通过 `LoadOrCreateNodeIdentity` 验证 StorageNode 首次启动会创建本地持久 identity，并在 reload 后复用同一个长期 `node_id`。
- 测试同时断言 StorageNode identity 不携带 `raft_id`，并使用 `NodeIdentitySource::kExplicitOverride` 作为来源诊断，避免把用例写成依赖 ViewNode 分配 ID。

## Boundary Checks

- 没有修改生产代码
- 没有修改 `modules/cluster/node_identity.h`
- 没有修改 `modules/cluster/node_identity.cpp`
- 没有修改 proto / 协议语义
- 没有修改 CMake
- 没有修改 app 文件
- 没有把 ViewNode 当成 Raft membership authority
- 没有把 StorageNode identity 写成 Raft voter

## Validation

- 构建命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

- 测试命令：

```bash
(
  flock -n 9 || exit 99
  ctest --preset debug-tests -R '^NodeIdentityTest\.' --output-on-failure
) 9>/tmp/cqupt_raft_build.lock
```

- 结果：`PASS`
- 说明：
  - `test_node_identity` 是 build target。
  - 当前仓库里的真实 CTest 入口是 `NodeIdentityTest.*`，因此按 case regex 运行，而不是把 `test_node_identity` 当作精确 test name。
- 通过摘要：
  - `test_node_identity` targeted build 成功
  - `NodeIdentityTest.*` 17/17 PASS
  - 总耗时约 `0.18 sec`

## Build Lock

- 使用了 `flock` 构建锁
- 已获得锁
- 本任务没有因锁竞争跳过 build/test

## Platform Notes

- Linux：已完成 targeted build + targeted CTest
- Windows：pending
- macOS：pending

## Risks / Follow-ups

- 当前生产 API 仍要求调用方传入 `identity_to_create.node_id`；T006 只能验证“缺失 identity_file 时可创建并持久化 caller-supplied StorageNode identity”，还不能证明“系统自动生成 node_id”。这属于后续 T011/T014 需要收口的实现边界。
- 当前 `NodeIdentity` 持久结构没有 `membership_state` 或 process `incarnation / boot epoch` 字段；T006 不能直接覆盖这些语义，需留给后续 T011/T013。
- 当前测试表面无法直接证明“identity 创建不进入 Raft log、不影响 quorum”；本任务通过“StorageNode 无 `raft_id`、测试仅调用本地 identity load/store/load-or-create API”间接锁定该边界，真正的系统级验证仍需后续 app / integration 任务补充。
- `tests/node_identity_test.cpp` 在本任务开始前已存在其他未提交编辑（T007/T008 相关 case）；本任务只在现状上叠加 T006，没有回退或覆盖这些改动。

## Result

- 最终状态：`PASS`
- 可以进入下一任务：`Yes`
- 下一步可进入：`T007` 或 `T011`
