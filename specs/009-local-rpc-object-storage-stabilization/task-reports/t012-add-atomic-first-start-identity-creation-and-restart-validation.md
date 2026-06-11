# T012 任务报告

## 做了什么

本任务收口并验证了 `NodeIdentity` 的 T012 边界：

- 确认 `LoadOrCreateNodeIdentity()` 在 `identity_file` 缺失时按 009 新格式创建 identity。
- 确认创建路径通过同目录临时文件、flush、原子 publish 和目录 durability 完成 first-start identity 落盘。
- 确认 `identity_file` 已存在时走 load + validation，而不是重新生成。
- 确认 old-format / corrupt / missing required field 都继续 fail-fast。
- 补了一个 residual staging file 回归测试，证明残留的 `node.identity.tmp.*` 不会被当成正式 identity。
- 修正了 `node_identity.h` 中过时的 T013 注释，明确 atomic publish / restart validation 已由 T012 提供。

## 修改了哪些文件

- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t012-add-atomic-first-start-identity-creation-and-restart-validation.md`

说明：

- 本任务没有修改 `tasks.md`。`speckit-implement` 流程通常会勾选任务状态，但当前用户任务明确禁止修改 `tasks.md`，因此本次遵守任务边界，不越界改任务文件。
- project setup verification 已完成：仓库是 git repo，存在 `.gitignore`；本任务未引入新的忽略规则需求，也不在允许修改范围内，因此未改 ignore 文件。

## first-start identity 创建现在如何保证原子 publish

当前 `modules/cluster/node_identity.cpp` 的 first-start 创建路径是：

1. `LoadOrCreateNodeIdentity()` 先调用 `LoadNodeIdentity()`。
2. 只有在 `node.identity` 真正不存在时，才进入 `StoreNodeIdentity()` 创建新 identity。
3. `StoreNodeIdentity()` 校验待写入 identity 的新格式语义后，调用 `PublishIdentityFile()`。
4. `PublishIdentityFile()` 在目标目录生成 staging 临时文件。
5. 写入完整 payload。
6. flush staging 文件：
   - Linux: `fsync(fd)`
   - Windows: `FlushFileBuffers(handle)`
7. 原子 publish：
   - Linux create-only: `link(staging, final)` 后 `unlink(staging)`
   - Linux replace-only: `rename(staging, final)`
   - Windows: `MoveFileExW(...)`
8. Linux required durability 下继续 `fsync(data_dir)`，把 “staging fsync -> 原子 publish -> 目录 fsync” 作为 durable 完成边界。

这样至少保证：

- 不会把半写入的正式 `node.identity` 当成合法 identity。
- create-only 并发下不会静默覆盖已有 identity。
- staging 残留不会被当成正式 identity，因为 load 只读取精确的 `node.identity` 路径。

## restart validation 现在覆盖哪些字段

当 `identity_file` 已存在时，`LoadNodeIdentity()` 现在会：

- 只读取精确的 `node.identity` 路径。
- 校验 `data_dir` 存在且是目录。
- 校验 `node.identity` 存在且是 regular file。
- 解析并要求新格式必填字段齐全。
- 对已加载 identity 做语义校验：
  - `cluster_id`
  - `node_id`
  - `node_type`
  - optional `raft_id`
  - `membership_state`
  - `persistent_generation`
  - `identity_version`
  - `source`
- 对调用方传入的 `ExpectedNodeIdentity` 做匹配校验：
  - `cluster_id` mismatch
  - `node_type` mismatch
  - `node_id` mismatch
  - Metadata bootstrap `raft_id` mismatch
  - optional `membership_state` mismatch
  - `source` mismatch
- 额外维持：
  - StorageNode / ViewNode 不允许携带 `raft_id`
  - Metadata dynamic join candidate 不允许通过 local override 持久化成 voter

## old-format / corrupt / missing required field 现在如何 fail-fast

当前行为：

- `identity_version != kNodeIdentityCurrentVersion`：
  - 视为 unsupported new-only schema，fail-fast。
- 缺少 `membership_state` / `persistent_generation` / 其他必填字段：
  - 解析阶段直接标为 corrupt，fail-fast。
- 非法 `membership_state` / 非法 `node_type` / 非法数字字段：
  - 解析阶段 fail-fast。
- `cluster_id` / `node_type` / `node_id` / `raft_id` 不匹配：
  - load 阶段返回 conflict 或 unsupported/corrupt 对应状态，不覆盖原文件。
- `LoadOrCreateNodeIdentity()`：
  - 只有 `NotFound` 才创建。
  - corrupt / unsupported / conflict 都原样返回，绝不把旧文件当 missing file 重建。

## 是否保留了 new-only identity 语义

保留。

本任务没有重新引入：

- legacy v1 compatibility
- silent auto-upgrade
- missing field default patch-up
- corrupt / old-format identity -> missing identity 的错误转换

`persistent_generation` 仍只表示当前新格式 identity 的 schema generation / diagnostics 字段，不承担旧格式兼容职责。

## 新增或改写了哪些测试

本任务新增：

- `T012FirstStartIgnoresResidualStagingFileAndCreatesFinalIdentity`
  - 在 data_dir 中预先放入 `node.identity.tmp.leftover`
  - 断言 first-start 仍会创建正式 `node.identity`
  - 断言 reload 复用正式 identity
  - 断言残留 staging 文件不会污染最终 identity 内容

本任务没有削弱 T006-T011 现有断言。

## T006-T011 测试是否通过

通过。

本轮 `NodeIdentityTest` 共 `27/27` PASS，其中包含：

- T006 first-start create
- T007 view first-start / restart reuse
- T008 metadata bootstrap voter
- T009 metadata dynamic join candidate
- T010 mismatch / corrupt fail-fast
- T011/T011-fix new-only identity format fail-fast
- T012 residual staging file 回归

## 构建和测试命令

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

测试：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\." --output-on-failure
```

兼容性残留检查：

```bash
grep -RniE "legacy|v1|old identity|old format|compatib|兼容|旧格式|新老版本|迁移|auto.?upgrade|自动升级" \
  modules/cluster/node_identity.h \
  modules/cluster/node_identity.cpp \
  tests/node_identity_test.cpp \
  specs/009-local-rpc-object-storage-stabilization
```

## 结果

- build: PASS
- test: PASS
- `NodeIdentityTest`: `27/27` passed

日志：

- `tmp/test-logs/t012-build.log`
- `tmp/test-logs/t012-ctest.log`

## 平台说明

- Linux：已验证 targeted build/test，通过。
- Windows：未实机验证，标记 pending。
  - 当前代码路径已提供 `CreateFileW` / `FlushFileBuffers` / `MoveFileExW` publish。
  - 但 required durability 下仍明确返回错误，因为目录 durability 未在 Windows 路径中宣称完成，避免 silent success。

## 是否可以进入 T013

可以。

T012 已经把 first-start atomic creation 和 restart validation 的 durable identity 边界收口。后续 T013 可以继续处理 process incarnation / boot epoch 与更细的 crash-safety / platform durability 后续边界。 
