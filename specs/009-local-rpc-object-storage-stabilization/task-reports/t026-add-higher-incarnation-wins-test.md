# T026 Add Higher Incarnation Wins Test

## 做了什么

本任务只修改测试，不修改任何 ViewNode registry 生产实现。

在 `tests/view_node_discovery_test.cpp` 中新增了一个更聚焦的 observed-state merge / incarnation ordering 测试，用来单独证明：

- 同一个 `node_id` 下，新进程实例的更高 `incarnation` 必须覆盖旧进程实例
- 旧 `incarnation` 即使带更高 `sequence` 和更晚 `observed_time`，也不能覆盖新 `incarnation`
- discovery / cluster view 最终保留的是新 `incarnation` 的 LIVE 状态，而不是旧 `incarnation` 的后到状态

## 修改了哪些文件

- `tests/view_node_discovery_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t026-add-higher-incarnation-wins-test.md`
- `specs/009-local-rpc-object-storage-stabilization/tasks.md`

## 新增测试名称

- `ViewNodeDiscoveryTest.HigherIncarnationWinsForViewNodeObservedState`

## 测试如何证明 higher incarnation wins

测试步骤：

1. 注册一个固定 `node_id=view-incarnation-order-1` 的 ViewNode 记录。
2. 先写入旧 `incarnation`：
   - `old_incarnation = view-incarnation-order-1:boot:110000000:10:1`
   - `sequence=5`
   - `observed_time=105`
3. 再写入新 `incarnation`：
   - `new_incarnation = view-incarnation-order-1:boot:111000000:11:1`
   - `sequence=1`
   - `observed_time=111`
4. 断言此时 registry snapshot 已切换到 `new_incarnation`
5. 再尝试写入旧 `incarnation` 的后到状态：
   - 仍然是 `old_incarnation`
   - `sequence=99`
   - `observed_time=120`，刻意比新状态更晚
   - `health=unavailable`，模拟旧实例坏状态
6. 断言这次写入被 `stale_ignored`
7. 对 `LookupNode()` 和 `GetClusterView()` 都断言最终保留：
   - `incarnation_id = new_incarnation`
   - `last_sequence = 1`
   - `last_seen_unix_ms = 111`
   - `health = healthy`
   - `liveness = LIVE`

这个测试直接证明：

- 更高 `incarnation` 是跨进程实例的主排序依据
- `observed_time` 不能单独成为跨 `incarnation` 的覆盖 authority
- 旧 `incarnation` 的后到状态不能把新 `LIVE` 状态打回去

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

日志：

- `tmp/test-logs/t026-build.log`

测试：

```bash
ctest --preset debug-tests -R "ViewNodeDiscoveryTest\\." --output-on-failure
```

结果：

- PASS
- `16/16` tests passed

其中新增测试：

- `ViewNodeDiscoveryTest.HigherIncarnationWinsForViewNodeObservedState`

日志：

- `tmp/test-logs/t026-ctest.log`

## 是否 PASS / FAIL / SKIPPED

PASS

本次没有因为构建锁、target 缺失或环境限制跳过 build/test。

## 是否已在 tasks.md 只勾选 T026 完成

是。

本次仅将 `tasks.md` 中的 T026 从 `[ ]` 改为 `[X]`，没有改动其他任务项，也没有在 `tasks.md` 写任何执行流水或说明。

## 是否可以进入后续任务

可以进入后续任务。

T026 现在已经把 “higher incarnation wins” 的核心语义固定成独立测试。后续：

- T027 可以继续单独收口 same-incarnation higher-sequence 语义
- T028 可以继续单独收口 observed_time-only stale override rejection 语义
