# T008 Add Metadata Bootstrap Voter Identity Tests

## Scope

本任务只新增 `Metadata bootstrap voter identity` 相关测试，不修改生产实现，不修改协议，不修改 Raft membership 逻辑。

## Files Changed

- `tests/node_identity_test.cpp`

## What Was Added

新增了两个聚焦 Metadata bootstrap voter 的测试：

1. `T008MetadataBootstrapVoterIdentityUsesFixedNodeIdAndRaftIdAcrossCreateAndReload`
   - 使用临时 identity file/data dir
   - 构造固定 `cluster_id`
   - 构造固定 `node_type=metadata`
   - 构造固定 `node_id`
   - 构造固定 `raft_id`
   - 验证首次创建成功
   - 验证 identity 文件落盘成功
   - 验证重载后 `node_id` 不变
   - 验证重载后 `raft_id` 不变
   - 验证当前 bootstrap voter 语义由 `metadata + positive raft_id + kConfigGenerator source` 表达

2. `T008MetadataBootstrapVoterIdentityRejectsDifferentExpectedRaftIdOnReload`
   - 先写入固定 bootstrap voter identity
   - 再使用不同 `raft_id` 期望重载
   - 验证返回 `kConflict`
   - 验证命中 `kRaftIdMismatch`

## Fixed node_id / raft_id Confirmation

本任务确认当前实现已经支持并可测试：

- bootstrap Metadata identity 使用固定 `node_id`
- bootstrap Metadata identity 使用固定正 `raft_id`
- 重启/重载后长期 `node_id` 保持不变
- 重启/重载后 `raft_id` 保持不变

## Membership State Coverage

当前 `modules/cluster/node_identity.h` 中的 `NodeIdentity` 尚未暴露 `membership_state` 字段，因此本任务无法直接断言 `membership_state=voter`。

本任务采用当前可表达的 bootstrap voter 等价语义：

- `node_type == metadata`
- `raft_id` 为固定正值
- `source == NodeIdentitySource::kConfigGenerator`

该缺口需要在后续 identity 扩展任务中继续收口。

## Bootstrap Voter vs Dynamic Join Candidate Boundary

本任务只测试静态 bootstrap initial voter 身份路径。

- 未把 dynamic join candidate 写成 voter
- 未把 ViewNode 观察状态当作 Metadata voter authority
- dynamic join candidate 场景留给 `T009`

## Related Existing Coverage Reused

`tests/cluster_config_test.cpp` 已经覆盖：

- Metadata `initial_role == voter`
- `voter_raft_ids`
- 固定 `raft_id`
- duplicate `raft_id` 冲突

因此本任务没有修改 `tests/cluster_config_test.cpp`，只在 `tests/node_identity_test.cpp` 补 identity 层面的 bootstrap voter 固定身份测试。

## Validation Commands

执行的 build 命令：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_node_identity cluster_config_test
) 9>/tmp/cqupt_raft_build.lock
```

执行的 ctest 命令：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\.|cluster_config_" --output-on-failure
```

## Validation Result

- build：PASS
- ctest：PASS
- 通过数量：29/29
- 总耗时：`0.22 sec`

补充说明：

- 用户建议的 `ctest --preset debug-ninja-low-parallel` 在当前仓库中不存在
- 通过 `ctest --preset debug-tests -N` 确认实际可用 test preset 后，改用 `debug-tests`
- 通过 `ctest -N` 确认实际 CTest test name 为 `NodeIdentityTest.*` 和 `cluster_config_*`，因此没有直接使用 binary target 名 `test_node_identity|cluster_config_test` 作为 `-R` 正则

## Next Step

如果 targeted build/test 通过，可以进入 `T009` 和后续 `T011`。如果失败，应先依据失败摘要修正测试假设或确认当前实现边界。
