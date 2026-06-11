# T013 任务报告

## 做了什么

本任务在 `modules/cluster` 中补齐了 process incarnation / boot epoch 生成边界，并保持它与长期 durable `NodeIdentity` 明确分离：

- 在 `node_identity.h/.cpp` 内新增 `ProcessIncarnation` 与 `ProcessIncarnationResult`。
- 新增集中入口 `CreateProcessIncarnation(const NodeIdentity&)`。
- 生成规则保证同一 `node_id` 的连续启动实例会得到不同的 `incarnation_id`。
- `startup_sequence_base` 明确固定为 `1`，供后续 heartbeat / self refresh / registry merge 任务作为同一 incarnation 内 sequence 起点使用。
- invalid identity 不会生成可用 incarnation。
- process incarnation 不写回 `node.identity`，也不影响 `membership_state`、Raft quorum 或 committed membership。

## 修改了哪些文件

- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `tests/node_identity_test.cpp`
- `specs/009-local-rpc-object-storage-stabilization/module-notes.md`
- `specs/009-local-rpc-object-storage-stabilization/task-reports/t013-add-process-incarnation-boot-epoch-generation-boundary.md`

说明：

- 本任务没有新增 `process_incarnation.cpp` / `.h` helper 文件。
- 本任务没有修改 CMake。
- 本任务没有修改 `tasks.md`。虽然 `speckit-implement` 通常会勾选任务，但当前任务约束明确禁止修改 `tasks.md`，因此本次遵守边界，不越界改任务文件。

## 是否新增 process incarnation helper

没有新增独立 helper 文件。

本次选择把 process incarnation 边界直接收敛在现有 `node_identity.*` 中，原因是：

- T013 只是在 durable identity 成功后生成单次启动实例身份；
- 逻辑规模较小；
- 不需要额外的跨文件 CMake wiring；
- 能直接复用现有 `NodeIdentity` 校验、诊断和状态码。

对外集中入口是：

```cpp
ProcessIncarnationResult CreateProcessIncarnation(const NodeIdentity &identity);
```

## incarnation / boot_epoch 的生成规则

当前生成规则：

1. 调用方必须先获得有效 `NodeIdentity`。
2. `CreateProcessIncarnation()` 先复用 `ValidateNodeIdentity()` 校验输入 identity。
3. 校验通过后，抓取当前 `system_clock` 时间：
   - `started_at_unix_ms`
   - 更高精度的 `started_at_unix_ns` 仅用于内部拼接唯一 `incarnation_id`
4. `incarnation_id` 由以下部分拼接：
   - `node_id`
   - `started_at_unix_ns`
   - 当前进程 pid
   - 进程内原子递增 ordinal
5. `startup_sequence_base` 固定为 `kProcessIncarnationInitialSequence`，当前值为 `1`

这样保证：

- 同一进程内连续调用不会碰撞；
- 同一 `node_id` 重启后会得到新的 `incarnation_id`；
- 测试不需要 `sleep`；
- 不依赖 ViewNode 分配；
- 不把 `observed_time` 当成实例身份。

## node_id 与 incarnation 的职责边界

- `node_id`
  - 长期逻辑身份
  - 存在 `node.identity`
  - 重启后复用
  - 可参与 Metadata durable identity 语义（例如 bootstrap voter 的固定 `raft_id` 关联）

- `ProcessIncarnation::incarnation_id`
  - 单次进程启动实例身份
  - 每次启动变化
  - 不写回 `node.identity`
  - 不等于 `membership_state`
  - 不参与 Raft quorum
  - 不改变 committed membership

- `observed_time`
  - 后续仅供 TTL / liveness / diagnostics
  - 不替代 incarnation ordering

## sequence 初始边界

本任务定义：

- `startup_sequence_base = 1`

边界含义：

- 后续 heartbeat / self refresh / registry merge 逻辑可以从 `1` 开始递增；
- `sequence` 不写进 `node.identity`；
- 跨进程重启比较不能使用 sequence，必须先比较 incarnation；
- 本任务不实现 sequence 递增 loop，只提供起点。

## 新增或修改了哪些测试

新增了以下 `NodeIdentityTest` 用例：

- `CreatesProcessIncarnationAfterFirstStartIdentity`
  - StorageNode first-start 创建 identity 后生成 incarnation
  - 断言 `node_id`、`incarnation_id`、`started_at_unix_ms`、`startup_sequence_base`
  - 断言 `incarnation` 不会被序列化回 `node.identity`

- `RestartReusesNodeIdButCreatesNewIncarnation`
  - StorageNode restart 复用长期 `node_id`
  - 断言新旧 `incarnation_id` 不同

- `ViewNodeRestartCreatesNewIncarnation`
  - ViewNode restart 复用长期 `node_id`
  - 断言新旧 `incarnation_id` 不同

- `MetadataBootstrapVoterKeepsRaftIdButChangesIncarnation`
  - Metadata bootstrap voter 保持长期 `node_id` / `raft_id`
  - 每次启动生成新的 incarnation

- `DynamicJoinCandidateIncarnationDoesNotPromoteToVoter`
  - Metadata dynamic join candidate 生成 incarnation 后仍然只是 `candidate`
  - 不会因为 process incarnation 变成 `voter`

- `InvalidIdentityDoesNotCreateProcessIncarnation`
  - 直接给 `CreateProcessIncarnation()` 传入非法 identity
  - 断言返回 `InvalidArgument` 且没有可用 incarnation

- `CorruptIdentityLoadDoesNotYieldUsableProcessIncarnation`
  - corrupt `node.identity` 的 load 失败时，不会得到可用于后续生成 incarnation 的 identity

本次没有削弱 T006-T012 的既有断言。

## 验证命令和结果

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

可选 grep：

```bash
grep -RniE "incarnation|boot_epoch|ProcessIncarnation|startup_sequence" \
  modules/cluster \
  tests/node_identity_test.cpp \
  specs/009-local-rpc-object-storage-stabilization/module-notes.md
```

## 结果

- build: PASS
- test: PASS
- `NodeIdentityTest`: `34/34` passed

日志：

- `tmp/test-logs/t013-build.log`
- `tmp/test-logs/t013-ctest.log`

## PASS / FAIL / SKIPPED

PASS。

本次没有因为构建锁、target 缺失或环境限制而跳过 build/test。

## Linux / Windows 说明

- Linux：已验证 targeted build/test，通过。
- Windows：未实机验证，标记 pending。
  - 本任务的 incarnation 生成逻辑只依赖标准库时间、pid 和进程内原子计数，设计上是平台中立的；
  - 但未在 Windows 环境下实际运行，不伪造通过。

## Project Setup Verification

已确认：

- 仓库是 git repo。
- `.gitignore` 已存在。

本任务没有新增 ignore 文件需求，也不在允许修改范围内，因此未修改 ignore 配置。

## 是否可以进入 T014

可以。

当前 `modules/cluster` 已同时提供：

- new-only durable `NodeIdentity`
- atomic first-start / restart validation
- process incarnation / boot epoch generation boundary

后续 T014 可以在 StorageNode app wiring 中，在 durable identity 成功 load/create 后生成并消费该 process incarnation。 
