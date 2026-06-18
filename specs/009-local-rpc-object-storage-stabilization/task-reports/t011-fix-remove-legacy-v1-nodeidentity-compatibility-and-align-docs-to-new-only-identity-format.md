# T011-fix 任务报告

## 做了什么

本次返工把 `NodeIdentity` 语义收敛为 009 唯一支持的新格式：

- 删除已有 `node.identity` load 路径上的 legacy v1 compatibility。
- 保留 first-start 创建、restart reuse、mismatch/corrupt fail-fast。
- 保留创建路径对新 identity 的默认 membership 归一化，但不再允许已存在 durable 文件在 load 时通过默认推断补齐 `membership_state`。
- 把 009 文档中“兼容旧格式 / 自动升级 / additive migration”相关表述改成 new-only identity format。

旧的 T011 报告里关于 “v1/v2 兼容加载” 的结论已被本返工推翻；该历史报告文件本身不在本次允许修改范围内，因此未直接改写旧报告内容。

## 修改了哪些文件

代码文件：

- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`

文档文件：

- `specs/009-local-rpc-object-storage-stabilization/data-model.md`
- `specs/009-local-rpc-object-storage-stabilization/plan.md`
- `specs/009-local-rpc-object-storage-stabilization/research.md`
- `specs/009-local-rpc-object-storage-stabilization/contracts/identity-lifecycle.md`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/validation-matrix.md`
- `specs/009-local-rpc-object-storage-stabilization/cross-task-risk-notes.md`

## 删除了哪些 legacy v1 compatibility 行为

- 不再接受 `identity_version=1` 作为受支持格式。
- 不再在 load 已存在 `node.identity` 时，根据 `node_type` / `source` / `raft_id` 自动推断并补齐 `membership_state`。
- 不再允许缺少 `membership_state` 的 identity 文件被当作可兼容输入。
- 不再允许缺少 `persistent_generation` 的 identity 文件被当作可兼容输入。
- 不再保留“旧格式静默升级”为新格式的行为。
- `LoadOrCreateNodeIdentity()` 遇到 old-format / missing-required-field / corrupt identity 时不再把它当作 missing file 重新创建。

实现上，当前策略是：

- create/store 路径仍可对调用方新建的 in-memory identity 进行默认 membership 归一化；
- load 路径只接受字段齐全、语义合法、版本受支持的 durable identity 文件。

## 新增或改写了哪些旧格式 fail-fast 测试

- `RejectsLegacyV1IdentityWithoutCompatibility`
  - 模拟旧 `v1` metadata identity，缺少 `membership_state` 与 `persistent_generation`。
  - 断言 `LoadNodeIdentity()` fail-fast。
  - 断言 `LoadOrCreateNodeIdentity()` 不会把旧文件当 missing file 自动重建。
  - 断言原文件内容不变。

- `MissingNewRequiredIdentityFieldsFailFast`
  - 模拟 `identity_version=2` 但缺少新必填字段的 identity 文件。
  - 断言 load fail-fast。
  - 断言 load-or-create 不会自动补字段或覆盖原文件。

- `LoadRejectsCorruptIdentityFile`
  - 改为显式测试当前新格式下的非法 `membership_state`，避免和 “legacy v1 缺字段” 语义混淆。

## T006-T010 语义是否仍保留

保留。

本次返工没有移除以下语义，相关 `NodeIdentityTest` 已继续通过：

- first-start missing identity creates new identity
- restart reuses stable `node_id`
- Metadata bootstrap voter 与 dynamic join candidate 分离
- cluster_id mismatch fail-fast
- node_type mismatch fail-fast
- Metadata `raft_id` mismatch fail-fast
- corrupt identity fail-fast
- dynamic Metadata candidate 不能通过本地文件成为 voter

## 文档语义改成了什么

009 文档现在统一表达为：

- 009 尚未正式部署旧 identity 格式。
- 009 只支持当前新格式 `node.identity`。
- old-format / unknown-format / missing-required-field / corrupt identity file 必须 fail-fast。
- 不做自动迁移、不做自动升级、不做静默补字段。
- `persistent_generation` 仅表示当前新格式 identity 的本地代际 / schema generation / diagnostics，不承担旧格式兼容职责。
- 如果未来真的需要线上迁移，应另开任务，不在 009 保留无部署依据的 legacy compatibility。

## 验证命令和结果

构建命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

结果：PASS

测试命令：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\." --output-on-failure
```

结果：PASS，`26/26` 通过，总耗时 `0.29 sec`

日志文件：

- `tmp/test-logs/t011-fix-build.log`
- `tmp/test-logs/t011-fix-ctest.log`

补充检查：

- 已执行 legacy/compatibility 关键词 grep。
- 当前残留命中主要分三类：
  - 本次新写入的否定语义，例如 “no legacy compatibility / fail fast”。
  - 历史任务报告 `t011-extend-...md` 中的旧结论；该文件不在本次允许修改范围。
  - `spec.md` 与部分历史 Phase 1 报告中的 “compatibility baseline / 迁移” 非 identity schema 正向兼容表述；本次未越界修改。

## 是否还有 T012 / T013 / T016 遗留事项

仍有后续任务边界，未在本次实现：

- T012：`node.identity` atomic publish / temp file / fsync / rename / directory durability。
- T013：更完整的 publish / crash-safety / platform durability 细节验证。
- T016：与 cluster config / runtime wiring 相关的后续边界收口。

本次没有进入：

- process incarnation
- ViewNode self refresh
- StorageNode dynamic join
- Metadata learner join
- Raft membership / quorum 变更

## 是否可以进入 T012

可以。

当前 `NodeIdentity` new-only schema 语义已经收敛，旧格式兼容路径已移除，identity 相关 targeted build/test 已通过，适合继续进入 T012 的 atomic publish 实现。 
