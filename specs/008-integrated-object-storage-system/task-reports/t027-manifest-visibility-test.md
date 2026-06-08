# T027 任务报告

## 1. 修改了哪些文件

- `tests/integrated_object_storage_e2e_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t027-manifest-visibility-test.md`

## 2. T027 的 PENDING hidden / COMMITTED visible 测试做了什么

- 在 `IntegratedObjectStorageE2ETest.ManifestVisibilityPendingHiddenCommittedVisible` 中新增 metadata 可见性测试。
- 通过真实 `raftdemo::MetadataStateMachine` 命令路径依次执行 `CreateBucket`、`CreateObject`、`CommitObject`，不绕过 `node_identity.cpp` 之外的生产路径，也不伪造 transfer/upload/download 逻辑。
- 在对象处于 `PENDING` 时验证：
  - `HeadObject` 返回 `kNotFound`
  - `ListObjects` 不返回该对象
  - `FindChunkRefs` 不返回 manifest chunk refs
- 在对象进入 `COMMITTED` 后验证：
  - `HeadObject` 返回可见对象
  - `ListObjects` 返回该对象
  - `FindChunkRefs` 返回 committed manifest 对应的 chunk refs
- 测试注释明确约束：对象普通可见性只能来自 MetadataNode 的 `COMMITTED` manifest，不能从 ViewNode 观测信息或 StorageNode 本地状态推断。

## 3. 是否有 disabled/scaffold 测试；如有，说明原因和后续启用条件

- 本任务新增的是可直接运行的 enabled 测试，不是 disabled/scaffold。
- 文件中仍保留 T026 的 disabled happy-path round-trip scaffold；那是前序任务内容，本任务未改变其启用条件。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 T027 锁定的是 metadata visibility 边界，尚未覆盖真实 transfer/download 路径；后续仍需要 T028 及 T029 之后的任务把同一约束贯穿到实际 client/adapter 流程。
- `specs/008-integrated-object-storage-system/tasks.md` 在本任务开始前已经存在 T026/T029/T031/T033 的未提交勾选变更；本任务只额外把 T027 从 `[ ]` 改为 `[X]`。
- 用户提供的构建 target 名称是 `integrated_object_storage_e2e`，但仓库当前实际 target 为 `test_integrated_object_storage_e2e`。
- `ctest --preset debug-tests` 与 `debug-ninja-safe` 构建目录不一致，可能出现 `No tests were found!!!`；如发生，需要显式指定 `--test-dir build/linux/safe`。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md；如未修改，明确说明未修改

- 未修改 `common-risk-notes.md`。
- 未修改 `risk-register.md`。

## 6. 验证命令和结果

- `git diff -- tests/integrated_object_storage_e2e_test.cpp tests/CMakeLists.txt tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t027-manifest-visibility-test.md`
  - 结果：确认本任务只改动了测试文件、T027 勾选和任务报告；`tests/CMakeLists.txt`、`tests/support/integrated_cluster_test_utils.h` 无本任务改动。
- `flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_integrated_object_storage_e2e'`
  - 结果：PASS，成功完成 configure，并成功编译 `test_integrated_object_storage_e2e`。
- `flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R \"IntegratedObjectStorageE2ETest\\.ManifestVisibilityPendingHiddenCommittedVisible\" --output-on-failure'`
  - 结果：命中已知目录不一致问题，CTest 在 `build/linux` 下返回 `No tests were found!!!`，不视为用例失败。
- 如 `debug-tests` preset 因目录不一致返回 `No tests were found!!!`，补充执行：
  - `flock -n /tmp/cqupt_raft_build.lock -c 'ctest --test-dir build/linux/safe -R \"IntegratedObjectStorageE2ETest\\.ManifestVisibilityPendingHiddenCommittedVisible\" --output-on-failure'`
  - 结果：PASS，`IntegratedObjectStorageE2ETest.ManifestVisibilityPendingHiddenCommittedVisible` 单测通过，`0 tests failed out of 1`。
