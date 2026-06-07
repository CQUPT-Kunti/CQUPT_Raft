# T014 任务报告：node identity 单元测试

## 1. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t014-node-identity-unit-tests.md`

补充说明：

- 未修改 `tests/CMakeLists.txt`，因为 `test_node_identity` 目标和 `RAFT_008_LABELS_NODE_IDENTITY` 标签入口已经存在。
- 未修改生产代码、proto、app 入口或测试 helper。

## 2. node identity 测试覆盖了什么边界

本次新增的 `tests/node_identity_test.cpp` 直接调用真实的：

- `StoreNodeIdentity`
- `LoadNodeIdentity`
- `LoadOrCreateNodeIdentity`
- `ResolveNodeIdentityPath`

没有绕过 `modules/cluster/node_identity.cpp` 的真实 load/store 路径。

覆盖的边界包括：

- 首次创建 identity
  - `StoreAndLoadMetadataIdentityRoundTrip`
  - 验证 MetadataNode identity 可以成功落盘并重新读取。

- 同一 `data_dir` 重启复用
  - `LoadOrCreateReusesExistingIdentityOnRestart`
  - 验证第一次创建成功，第二次不会伪造新身份，而是复用原 identity。

- identity 与 expected 配置冲突时失败
  - `LoadReportsConflictWhenExpectedNodeIdMismatches`
  - 验证读取时 `node_id` mismatch 返回 `kConflict`，并带 `kNodeIdMismatch` 诊断。

- 损坏文件诊断
  - `LoadRejectsCorruptIdentityFile`
  - 手工写入缺字段的 `node.identity`，验证返回 `kCorrupt` 和 `kIdentityFileCorrupt`。

- MetadataNode / StorageNode 的 `raft_id` 边界
  - `StoreRejectsStorageIdentityThatCarriesRaftId`
  - `StoreRejectsMetadataIdentityWithoutRaftId`
  - 验证 StorageNode 不得伪造 `raft_id`，MetadataNode 必须提供 `raft_id`。

- 不静默覆盖已有 identity
  - `CreateOnlyModeDoesNotSilentlyOverwriteExistingIdentity`
  - 验证 `CreateNewOnly` 模式再次写入时返回 `kConflict`。

- 非法路径 / 非目录 `data_dir`
  - `LoadRejectsDataDirThatIsNotDirectory`
  - 验证把普通文件当作 `data_dir` 时返回明确错误。

- required durability 不得 no-op success
  - `RequiredDurabilityDoesNotSilentlySucceed`
  - Linux 断言 `required` 模式成功且 `durable=true`
  - Windows 断言 `required` 模式返回 `kDurabilityError`，不把弱保证伪装成成功

这些测试保持平台中立，同时对 Linux / Windows 差异做了显式断言边界。

## 3. 是否发现不合理点 / 警告 / 风险

- 当前 `test_node_identity` 目标虽已在 `tests/CMakeLists.txt` 预留，但本窗口未能完成 build/test，因为构建锁被占用。
- `tasks.md` 在本任务开始前已经存在其他未提交勾选变更；本任务只新增 T014 的 `[X]`。
- `node_identity.cpp` 的 Windows `required` durability 设计为显式失败，这在测试里被当作正确 contract 验证，不应被误解为实现缺陷。

## 4. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。

## 5. 验证命令和结果

执行：

```bash
git diff -- tests/node_identity_test.cpp tests/CMakeLists.txt specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t014-node-identity-unit-tests.md
```

结果：PASS。

补充说明：

- `git diff -- <path>` 不会直接展示未跟踪新文件的完整内容，但可确认 `tasks.md` 修改范围以及 `tests/CMakeLists.txt` 没有本任务变更。
- 本任务新增文件为 `tests/node_identity_test.cpp` 和本报告文件。

执行：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target node_identity_test' \
  || echo "build lock busy, skip build in this window"
```

本窗口实际执行命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c "cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target test_node_identity" || echo "build lock busy, skip build in this window"
```

结果：

- `build lock busy, skip build in this window`

说明：

- 构建锁被占用，本窗口未执行 build。
- 按任务要求，没有等待，也没有重复启动第二次构建。

测试命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'ctest --preset debug-tests -R node_identity --output-on-failure' \
  || echo "build/test lock busy, skip test in this window"
```

结果：

- 由于构建锁已占用且未完成本窗口 build，本窗口未执行 test。

结论：

- 当前已完成 T014 所需测试代码和任务回填。
- build/test 待后续统一验证窗口持锁执行。
