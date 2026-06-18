# T027 任务报告

## 做了什么

本任务只在 `tests/view_node_discovery_test.cpp` 新增了 same-incarnation sequence 排序测试，没有修改任何 ViewNode registry 生产实现。

新增测试专门锁定这条边界：

- 同一个 `node_id`
- 同一个 `incarnation`
- 更高 `sequence` 的 observed state 必须覆盖更低 `sequence`
- 更低 `sequence` 即使带更晚的 `observed_at_unix_ms`，也不能回滚最终快照

## 修改了哪些文件

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t027-add-same-incarnation-higher-sequence-wins-test.md`

## 新增测试名称

- `ViewNodeDiscoveryTest.HigherSequenceWinsWithinSameIncarnation`

## 测试如何证明 same-incarnation higher sequence wins

测试流程：

1. 注册一个 ViewNode self record。
2. 用同一个 `incarnation_id` 写入 `sequence=10` 的状态。
3. 再用同一个 `incarnation_id` 写入 `sequence=11` 的状态。
4. 断言快照保留：
   - `last_sequence=11`
   - `last_seen_unix_ms=111`
   - `health=healthy`
5. 再尝试写入同一个 `incarnation_id` 下的旧状态：
   - `sequence=10`
   - 更晚的 `observed_at_unix_ms=250`
   - 更差的 `health=unavailable`
6. 断言这次更新返回 `kStaleIgnored`，且最终 lookup 仍保留 sequence=11 的状态，不会被晚到但更低 sequence 的状态覆盖。

说明：

- 当前项目的 `liveness` 是 TTL 推导值，不是请求直接携带的输入状态。
- 因此本测试用“更晚 observed_time + 更差 health + 旧 sequence 仍不能覆盖”的方式表达 T027 当前可验证的同 incarnation 顺序边界。

## 验证命令和结果

构建：

```bash
(
  flock -n 9 || exit 99
  cmake --build --preset debug-ninja-low-parallel --target test_view_node_discovery
) 9>/tmp/cqupt_raft_build.lock
```

结果：

- PASS

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscovery" --output-on-failure
```

结果：

- PASS
- `17/17` tests passed

日志：

- `tmp/test-logs/t027-build.log`
- `tmp/test-logs/t027-ctest.log`

## PASS / FAIL / SKIPPED

PASS

本次没有因为构建锁、target 缺失或环境限制跳过。

## tasks.md

本任务验证通过后，已仅将 `tasks.md` 中的 T027 从 `[ ]` 改为 `[X]`。

## 是否可以进入后续任务

可以。

T027 的测试边界已经单独锁定；后续可以继续做：

- T028：`observed_time` 不能单独覆盖状态
- T030/T031：把这些排序规则完整收口到生产 merge 逻辑中
