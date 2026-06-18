# T010 Add Mismatch And Corrupt Identity Fail-Fast Tests

## 做了什么

本任务只补 `identity mismatch / corrupt file` 的 fail-fast 测试，不修改生产实现。

本次测试增强重点放在 `LoadOrCreateNodeIdentity(...)` 路径，因为 T010 需要证明：

- mismatch 不能被误当成 first-start missing identity；
- corrupt identity file 不能被误当成 missing file 自动生成新身份；
- 旧 `node.identity` 文件在 fail-fast 后必须保留，不能被覆盖。

## 修改了哪个文件

- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t010-add-mismatch-and-corrupt-identity-fail-fast-tests.md`

## 新增了哪些 mismatch / corrupt fail-fast 测试

### `T010ClusterIdMismatchFailsFastAndDoesNotOverwriteExistingIdentity`

覆盖点：

- 先创建合法 Storage identity file。
- 再用不同 `cluster_id` 的配置执行 `LoadOrCreateNodeIdentity(...)`。
- 断言返回 `kConflict`。
- 断言命中 `kClusterIdMismatch`。
- 断言诊断包含 `cluster_id mismatch`，且同时包含 expected/actual cluster 值。
- 断言旧 identity 文件内容完全不变，没有被新的 identity 请求覆盖。

### `T010NodeTypeMismatchFailsFastAndDoesNotRewriteExistingIdentity`

覆盖点：

- 先创建合法 Storage identity file。
- 再以 `node_type=metadata` 的配置加载同一 identity。
- 断言返回 `kConflict`。
- 断言命中 `kNodeTypeMismatch`。
- 断言诊断包含 `node_type mismatch`，且能看出 `storage` / `metadata` 维度。
- 断言旧 identity 文件未被重写，重新按原 Storage 期望加载仍成功。

### `T010MetadataRaftIdMismatchFailsFastAndPreservesBootstrapIdentity`

覆盖点：

- 先创建合法 Metadata bootstrap voter identity，带固定 `raft_id=77`。
- 再以不同 `raft_id=99` 的配置执行 `LoadOrCreateNodeIdentity(...)`。
- 断言返回 `kConflict`。
- 断言命中 `kRaftIdMismatch`。
- 断言诊断包含 `raft_id mismatch`，且能看出 expected/actual raft_id。
- 断言旧 identity 文件内容不变，重新按原 `raft_id=77` 加载仍成功。

### `T010CorruptIdentityFileFailsFastAndIsNotTreatedAsMissingOnLoadOrCreate`

覆盖点：

- 手工写入非法格式的 `node.identity`：
  - 前三行是合法字段
  - 第四行是非 `key=value` 的损坏内容
- 调用 `LoadOrCreateNodeIdentity(...)`。
- 断言返回 `kCorrupt`。
- 断言命中 `kIdentityFileCorrupt`。
- 断言诊断包含有意义的位置线索 `line[4]`。
- 断言损坏文件仍然存在且内容完全不变，没有被新 identity 覆盖。
- 再次直接 `LoadNodeIdentity(...)`，确认同一文件仍被识别为 corrupt，而不是被当成 missing file。

## 哪些断言当前通过，哪些暴露生产实现缺口

当前通过的断言：

- `cluster_id mismatch` 会 fail-fast。
- `node_type mismatch` 会 fail-fast。
- `Metadata raft_id mismatch` 会 fail-fast。
- `corrupt identity file` 会 fail-fast。
- 上述四类失败都不会触发自动生成新 identity 覆盖旧文件。
- 诊断信息能指向 `cluster_id` / `node_type` / `raft_id` / corrupt 行号等问题维度。

沿用现有测试并继续有效的断言：

- `LoadReportsConflictWhenExpectedNodeIdMismatches`
  - 覆盖 `node_id mismatch` 的基础 fail-fast。
- `T069StorageNodeIdentityMismatchFailsAndKeepsExistingIdentity`
  - 覆盖 `cluster_id + node_id` 组合 mismatch 不覆盖旧文件。
- `LoadRejectsCorruptIdentityFile`
  - 覆盖直接 `LoadNodeIdentity(...)` 的 corrupt fail-fast。
- `T008MetadataBootstrapVoterIdentityRejectsDifferentExpectedRaftIdOnReload`
  - 覆盖直接 `LoadNodeIdentity(...)` 的 `raft_id mismatch`。

本任务没有暴露新的 production 缺口：

- 当前实现已经支持 `cluster_id` / `node_type` / `node_id` / `raft_id` mismatch 的显式 fail-fast。
- 当前实现已经支持 corrupt identity file 的显式 fail-fast。
- 当前实现的 fail-fast 路径不会把 mismatch/corrupt 误判成 missing file。

补充说明：

- T009 中记录的 `membership_state` / optional provisional `raft_id` 缺口依然存在，但那是 dynamic join candidate 表达能力的问题，不是 T010 的 mismatch/corrupt fail-fast 缺口。

## 验证命令和结果

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity
) 9>/tmp/cqupt_raft_build.lock
```

结果：

- PASS
- 总耗时：约 `3s`
- 日志：`tmp/test-logs/t010-build.log`

测试：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\." --output-on-failure
```

结果：

- PASS
- 24/24 passed
- 总耗时：`0.26 sec`
- 日志：`tmp/test-logs/t010-ctest-debug-tests.log`

补充说明：

- 当前仓库不存在 `ctest --preset debug-ninja-low-parallel` 这个 test preset。
- 已使用当前仓库实际存在的 `debug-tests` preset 完成定向验证。

## 是否可以进入 T011

可以。

T010 已经把 identity mismatch / corrupt file 的 fail-fast 测试边界补齐到可支撑后续实现的程度。下一步可以进入 `T011`，在不回退这些 fail-fast 约束的前提下扩展：

- `membership_state`
- optional/provisional `raft_id`
- persistent generation

以及对应的数据模型与实现。 
