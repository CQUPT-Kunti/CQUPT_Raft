## T031 执行结果

### 1. 做了什么

在 `modules/view/view_registry.cpp` 中把 observed state merge ordering 收敛为显式的确定性决策逻辑，确保同一节点的多份观测状态在 merge 时稳定按 incarnation 和 sequence 判定新旧，不再依赖隐式回退。

### 2. 修改了哪些文件

- `modules/view/view_registry.cpp`
- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t031-implement-deterministic-observed-state-merge-ordering.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`（仅在 PASS 后将 T031 标记完成）

### 3. merge ordering 现在如何工作

- `CompareIncarnationIds(...)` 现在对以下情况给出确定性结果：
  - 两边都没有 incarnation：视为同一代，继续按 sequence 比较
  - 现有记录没有 incarnation、传入记录有 incarnation：传入更新更“新”
  - 现有记录有 incarnation、传入记录没有 incarnation：传入更新更“旧”
  - 两边都有 incarnation 且可解析：按 `boot/start_ns + pid + ordinal` 比较
  - 两边都有 incarnation 但无法按结构比较：退化为原始字符串的确定性比较，不再回到不稳定的 observed_time 逻辑
- `DetermineObservedStateMergeDecision(...)` 统一处理 merge：
  - higher incarnation wins
  - same incarnation higher sequence wins
  - lower sequence stale ignore
  - observed_time 只在 incarnation 层级允许后，作为同代 sequence 比较的辅助事实
- 被拒绝的旧状态不会修改当前 registry record。

### 4. discovery / liveness 是否保持

- 保持不变。
- discovery、lookup、cluster view、self refresh、TTL/liveness 推导都继续使用当前 accepted observed state。
- 没有把 ViewNode 变成 membership authority，也没有改动任何 Raft membership 语义。

### 5. 新增或更新了哪些测试

保留并通过了 T026-T028 相关测试，同时新增：

- `ViewNodeDiscoveryTest.MissingIncarnationCannotOverrideIncarnationAwareCurrentState`

这个测试验证：

- 当 registry 当前状态已经是 incarnation-aware 时
- 一个更晚、sequence 更高、但缺失 incarnation 的 heartbeat
- 也不能覆盖当前 live 状态
- 且被拒绝后 lookup 读回的 registry record 不变

### 6. 验证命令和结果

构建命令：

```bash
( flock -n 9 || exit 99; cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery ) 9>/tmp/cqupt_raft_build.lock
```

结果：PASS

测试命令：

```bash
ctest --preset debug-tests -R 'ViewNodeDiscovery' --output-on-failure
```

结果：PASS（19/19）

### 7. 最终状态

- 状态：PASS

### 8. 后续边界

- 本任务只实现 registry 内 deterministic merge ordering。
- 未实现 ViewNode peer sync 网络同步。
- 未改动 StorageNode dynamic join、Metadata learner join、Raft membership / quorum。

### 9. tasks.md 勾选情况

- 在验证 PASS 后，仅将 `T031` 从 `[ ]` 改为 `[X]`。

### 10. 后续任务可行性

- 可以进入 T032。
- T032 可以在当前确定性排序基础上继续补充冲突诊断和更细的 registry 误用信息。
