# T009 Add Metadata Dynamic Join Candidate Identity Tests

## 做了什么

本任务只新增 `Metadata dynamic join candidate identity` 相关测试，不修改生产实现，不修改协议语义，不修改持久化格式。

测试策略按当前 `modules/cluster/node_identity.*` 的可表达边界收口：

- 正向覆盖首次启动时 `identity_file` 缺失、dynamic join candidate identity 创建成功、identity 文件落盘成功。
- 负向覆盖 candidate local identity 不能被当成 bootstrap voter identity 复用。
- 用一个缺口测试固定当前实现仍要求 Metadata identity 提供正 `raft_id`，因此尚不能表达“空 raft_id / 未提交 raft authority”的 009 合同语义。

## 修改了哪个文件

- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t009-add-metadata-dynamic-join-candidate-identity-tests.md`

## 新增了哪些 Metadata dynamic join candidate identity 测试

### `T009MetadataDynamicJoinCandidateFirstStartCreatesIdentityFileWithoutBootstrapAuthorityMarkers`

覆盖点：

- 使用临时目录和初始不存在的 `node.identity`。
- 构造明确的 Metadata dynamic join candidate 输入：
  - `node_type=metadata`
  - `source=NodeIdentitySource::kExplicitOverride`
  - 非 bootstrap voter 的本地 candidate provenance
- 首次创建成功。
- `identity_file` 被写入。
- `cluster_id` 正确。
- `node_type=metadata`。
- `node_id` 非空且与输入一致。
- 当前实现下使用正 `raft_id=301` 作为 provisional/local-only 占位表达。
- 直接读取落盘文件，确认当前格式没有 `membership_state=`、`initial_role=` 或 `voter` 标记，避免把本地 identity 文件误当成 committed membership authority。

### `T009MetadataDynamicJoinCandidateCannotBeReloadedAsBootstrapVoterFromLocalFile`

覆盖点：

- 先创建 dynamic join candidate identity。
- 再以 `source=kConfigGenerator` 的 bootstrap voter 期望重新加载同一个 identity 文件。
- 断言返回 `kConflict`。
- 断言命中 `kSourceMismatch`。

该测试证明了当前实现至少不会把“dynamic join candidate 的本地 identity 文件”静默重解释为“bootstrap voter identity”。

### `T009MetadataDynamicJoinCandidateWithoutRaftIdDocumentsCurrentGapForUncommittedIdentity`

覆盖点：

- 构造 dynamic join candidate，但不提供 `raft_id`。
- 断言当前实现返回 `kInvalidArgument`。
- 断言命中 `kMissingRaftId`。
- 断言没有写出 `identity_file`。

该测试不是在证明目标语义已完成，而是在固定当前缺口：009 合同允许 candidate `raft_id` 为空、provisional，或保持未提交状态，但当前 production 仍要求 Metadata identity 提供正 `raft_id`。

## 是否证明 candidate 不能成为 voter

部分证明，按当前实现的可表达语义成立：

- 已证明 candidate local identity 不会因为本地文件而被重载为 bootstrap voter provenance。
- 已证明当前 `node.identity` 文件格式不包含 `membership_state` / `initial_role` / `voter` authority 标记。

但还不能完全证明“candidate 绝不可能通过本地文件成为 voter”，原因是当前 production 还没有：

- 持久 `membership_state`
- committed membership authority 字段
- 区分 bootstrap voter / dynamic join candidate / learner / voter 的完整本地状态机

因此本任务把“不能通过本地文件自我晋升为 voter”的当前最强可测边界固定为：

- local file provenance 不能伪装成 bootstrap voter provenance
- local file 不携带 committed membership authority

## 当前 production 是否缺少 membership_state / provisional raft_id 表达

是，当前缺少，且已经在测试中显式记录：

- `NodeIdentity` 当前没有 `membership_state` 字段。
- 当前 `tests/node_identity_test.cpp` 只能通过 `source` 和落盘文件内容缺少 authority 标记来近似表达 “joining/candidate but not voter”。
- 当前 production 仍要求 Metadata identity 必须带正 `raft_id`，因此还不能表达：
  - `raft_id` 为空
  - `raft_id` provisional but not yet committed
  - `membership_state=joining/candidate`
- “是否成为 learner / voter 必须由 Metadata leader 通过 committed Raft membership change 决定” 这一点目前不在 `node_identity` 数据模型中落盘，只能留给后续 `T011` / `T016` / `T055` 收口。
- “ViewNode 只能观察，不能决定 voter” 目前也没有专门的 identity 字段可直接断言；本任务仅证明 `node.identity` 文件本身不携带 voter authority。

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
- 日志：`tmp/test-logs/t009-build.log`

测试：

```bash
ctest --preset debug-tests -R "NodeIdentityTest\\." --output-on-failure
```

结果：

- PASS
- 20/20 passed
- 总耗时：`0.23 sec`
- 日志：`tmp/test-logs/t009-ctest-debug-tests.log`

补充说明：

- 当前仓库不存在 `ctest --preset debug-ninja-low-parallel` 这个 test preset。
- 已通过 `ctest --list-presets` 确认可用 preset 后，使用实际存在的 `debug-tests` 完成定向验证。

## 是否可以进入 T010 / T016 / T055

可以，但要带着当前 identity model 缺口继续推进：

- 可以进入 `T010`，继续补 mismatch / corrupt fail-fast 测试。
- 可以进入 `T016`，把 Metadata bootstrap vs dynamic join identity mode 真正接入 app 启动路径。
- 可以进入 `T055`，补 `cluster_config_test` / `node_identity_test` 上更完整的 dynamic candidate identity/config 测试。

需要注意：

- `membership_state`
- learner/voter committed authority
- empty/provisional `raft_id`

这些仍然需要后续生产实现和测试一起补齐。
