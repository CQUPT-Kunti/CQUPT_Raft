## T029 执行结果

### 1. 做了什么

在 `tests/node_identity_test.cpp` 新增 restart old-incarnation rejection 测试，只使用测试侧局部 helper 表达“同一持久 node_id 在重启后会生成新的 process incarnation，旧 incarnation 不再是当前实例”这一边界。

### 2. 修改了哪些文件

- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t029-add-identity-restart-old-incarnation-rejection-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`（仅在 PASS 后将 T029 标记完成）

### 3. 新增测试名称

- `NodeIdentityTest.RestartReusesNodeIdButRejectsOldIncarnation`

### 4. 测试如何证明 restart 后旧 incarnation 不再是当前状态

- 首次启动创建并加载同一个 ViewNode identity，生成 `first_incarnation`。
- 模拟 restart：重新从相同 `identity_file` 加载长期 identity，再生成 `current_incarnation`。
- 断言长期 `node_id`、`cluster_id`、`node_type` 保持不变。
- 断言 `current_incarnation.incarnation_id != first_incarnation.incarnation_id`。
- 断言 `incarnation_id` 中 `:boot:` 后的 boot token 发生变化，证明 restart 后当前进程实例可与旧实例区分。
- 断言测试侧 `IsCurrentIncarnation(first_incarnation, current_incarnation) == false`，表达旧 incarnation 已不能再被当作 current incarnation 使用。

说明：

- `node_identity` 层当前负责“生成并区分” process incarnation，不直接承担 ViewNode registry 对旧 heartbeat / self refresh / observed state 的 reject 逻辑。
- 旧 observed state 被更高 incarnation 拒绝/忽略的合并语义由 `tests/view_node_discovery_test.cpp` 中的高 incarnation merge 测试覆盖。

### 5. 验证命令和结果

构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_node_identity ) 9>/tmp/cqupt_raft_build.lock
```

结果：PASS

测试命令：

```bash
ctest --preset debug-tests -R 'NodeIdentityTest\\.' --output-on-failure
```

结果：PASS（35/35）

补充：

- 用户建议中的 `ctest --preset debug-ninja-low-parallel ...` 在当前仓库不存在对应 test preset，因此按仓库真实可用 preset 改为 `debug-tests`。

### 6. 最终状态

- 状态：PASS
- 已满足“restart 复用长期 node_id，但旧 incarnation 不再是当前实例”的测试表达。

### 7. tasks.md 勾选情况

- 在验证 PASS 后，仅将 `T029` 从 `[ ]` 改为 `[X]`。

### 8. 后续任务可行性

- 可以进入后续任务。
- 对于“旧 heartbeat / self refresh / observed state 在 registry merge 中被拒绝”的完整行为，后续继续由 T030-T031 及 `view_node_discovery` 相关测试收口。
