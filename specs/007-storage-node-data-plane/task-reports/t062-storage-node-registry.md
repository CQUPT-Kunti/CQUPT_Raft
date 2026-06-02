# T062 Storage Node Registry

## 修改文件

- `modules/store/node/storage_node_registry.h`
- `modules/store/node/storage_node_registry.cpp`
- `modules/store/node/module-notes.md`
- `modules/store/node/AGENTS.md`
- `modules/store/AGENTS.md`
- `CMakeLists.txt`
- `tests/storage_heartbeat_registry_test.cpp`
- `specs/007-storage-node-data-plane/task-reports/t062-storage-node-registry.md`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 新增生产 in-memory `StorageNodeRegistry`，支持：
  - `RegisterStorageNode`
  - `UpdateStorageNodeHeartbeat`
  - `ReportHealth`
  - `ReportCapacity`
  - `ReportLoad`
  - `LookupNode`
  - `ListNodes`
  - `Snapshot`
- 为 registry 定义了 facts、请求、结果和 liveness 类型，统一保存 capacity、health、disk pressure、io_error_count、load、failure-domain、last_sequence、last_seen。
- 将 `tests/storage_heartbeat_registry_test.cpp` 从 T059 的 test-only adapter 迁移到生产 `StorageNodeRegistry`，覆盖注册、幂等、冲突、heartbeat 更新、stale/same-sequence 保护、partial merge、liveness 和稳定排序。
- 更新 `modules/store/node/module-notes.md`、`modules/store/node/AGENTS.md`、`modules/store/AGENTS.md`，补齐 registry 职责与边界说明。

## StorageNodeRegistry 输入、输出和状态语义

- `RegisterStorageNode`
  - 输入：`node_id`、`endpoint`、`observed_at_unix_ms`、初始 facts
  - 输出：`created/idempotent/snapshot`
  - 语义：同 `node_id + endpoint` 幂等；同 `node_id` 不同 endpoint 冲突；默认启用 endpoint 全局唯一性保护
- `UpdateStorageNodeHeartbeat`
  - 输入：全量 facts、`sequence`、`observed_at_unix_ms`
  - 输出：`accepted_sequence/applied/idempotent/stale_ignored/snapshot`
  - 语义：更小 sequence 或更旧 observed time 不能覆盖更新 facts；同 sequence 幂等；成功时整体替换 facts
- `ReportHealth` / `ReportCapacity` / `ReportLoad`
  - 输入：局部 facts、`sequence`、`observed_at_unix_ms`
  - 输出：统一复用 update result
  - 语义：只覆盖对应事实分组，不清掉未上报的其它 facts
- `LookupNode` / `ListNodes` / `Snapshot`
  - 输出：带 `last_sequence`、`last_seen_unix_ms`、`liveness` 和 facts 的稳定快照
  - 语义：`ListNodes` / `Snapshot` 按 `node_id` 稳定排序

## sequence / stale heartbeat / liveness 当前边界

- sequence 规则：
  - `incoming_sequence < last_sequence` => `kAlreadyExists` + `stale_ignored`
  - `incoming_sequence == last_sequence` => `kOk` + `idempotent`
  - `incoming_sequence > last_sequence` 但 `observed_at_unix_ms < last_seen_unix_ms` => `kAlreadyExists` + `stale_ignored`
  - 其它情况 => 应用更新
- liveness 规则：
  - `elapsed <= stale_timeout_ms` => `kLive`
  - `elapsed <= dead_timeout_ms` => `kStale`
  - 否则 => `kDead`
- 当前边界：
  - registry 使用调用方传入的 `observed_at_unix_ms` 和查询时传入的 `now_unix_ms`
  - 不在 T062 内引入独立时钟源、RPC 时间源或跨节点 clock 校正

## 是否调用 metadata / Raft；是否保存 payload

- 不调用 metadata
- 不调用 Raft
- 不保存 payload
- registry 只保存 StorageNode data-plane facts 和派生出来的 liveness/snapshot

## 是否使用 tests/test_file/test_file.zip

- 否

## 验证命令、PASS/FAIL、日志路径

- `mkdir -p tmp/007`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `ctest --test-dir build/linux -R "storage_heartbeat_registry|storage_node_registry|heartbeat_registry" --output-on-failure 2>&1 | tee tmp/007/t062-storage-node-registry.log`
  - PASS
  - 实际匹配到的测试名为 `storage_heartbeat_registry`
  - 日志路径：`tmp/007/t062-storage-node-registry.log`

## 如果失败：失败原因、失败测试名、错误摘要、最后 50 行日志

- 本次验证未失败

## Windows 验证判断

- T062 是平台无关的 in-memory registry 与单元测试任务，一般不单列 `T062-WIN`
- 当前无 Windows 编译/测试环境，不伪造 Windows PASS
- 本任务未引入平台相关文件、网络或系统调用行为

## 是否通过 T062

- 是

## 是否可以进入 T063

- 可以
- 前提：后续 heartbeat/report/register gRPC service 入口必须复用 T062 已固定的 sequence、stale 和 partial merge contract，不能回退到 test-only adapter 语义

## 当前任务发现的不合理点 / 警告 / 风险

- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks` 仍错误返回 `specs/006-remove-kv-metadata-state-machine`，执行 `007` 任务时需要人工纠偏
- 当前 registry 对 load facts 只做结构化保存，不在 T062 内发明 overload 生产打分；最终消费规则仍待 T065/T066 收口
- 当前 freshness/liveness 仍依赖调用方提供一致的时间与 sequence 语义，service/client 接线前该风险不能关闭

## 是否更新 module-notes.md / AGENTS.md

- 是
- `modules/store/node/module-notes.md`：补充 registry 职责、边界和 `.cpp` helper
- `modules/store/node/AGENTS.md`：补充 node 模块现在也承接 in-memory registry facts
- `modules/store/AGENTS.md`：补充 `node/` 子模块包含 registry 职责

## module-notes.md 是否补充 .cpp 关键函数 / helper

- 是
- 已补充：
  - register helper
  - heartbeat sequence / stale 判断 helper
  - partial report merge helper
  - liveness 推导 helper
  - snapshot/list 稳定排序 helper
  - validation helper

## 是否修改高频文档及原因

- 修改了 `specs/007-storage-node-data-plane/tasks.md`
  - 原因：将 T062 标记完成，并记录实际影响文件
- 修改了 `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
  - 原因：收缩“生产 registry 未实现”风险，并新增 T062 后仍保留的 clock/sequence/facts-consumer 风险

## common-risk-notes.md 读取结果

- 已读取并维护
- 继续保留 T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T045、T049、T055、T056、T057、T059、T060、T061 风险

## common-risk-notes.md 新增/删除/保留情况

- 新增：`T062` 生产 registry 已完成，但真实时间源、sequence 生产策略和 placement/read-side 消费仍待后续任务收口
- 删除：无整项删除
- 收缩：
  - `T059` 从“没有生产 registry”收缩为“生产 registry 已有，但 service/client/placement/read-side 未接线”
  - `T061` 从“schema 已有但 registry 未落地”收缩为“schema + registry 已有，但 RPC/consumer 接线未完成”
