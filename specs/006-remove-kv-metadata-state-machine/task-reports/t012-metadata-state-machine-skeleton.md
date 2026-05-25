# T012 MetadataStateMachine 空骨架和最小编译测试

## 1. 结论

- T012 已完成。
- 本次没有替换默认 `RaftNode` wiring，没有删除 KV，没有修改 `StrongConsistencyMetadataStateMachine` 现有行为路径。
- 本次是在现有 metadata 状态机文件中新增并行的 `MetadataStateMachine` 骨架类型，用于后续 V2 apply / query / snapshot/recovery 迁移。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.h`
  - 新增 `MetadataStateMachine : public IStateMachine`
  - 新增 `MetadataHeadObjectResponse`
  - 新增 `MetadataListObjectsResponse`
  - 新增最小查询占位接口：
    - `HeadObject(const HeadObjectQuery&)`
    - `ListObjects(const ListObjectsQuery&)`
  - 新增最小状态访问器：
    - `LastAppliedIndex()`
    - `LastAppliedTerm()`
    - `BucketCount()`
    - `ObjectCount()`
    - `RequestCount()`
    - `TombstoneCount()`
  - 预留内部表结构：
    - `buckets_`
    - `objects_`
    - `object_index_`
    - `chunk_ref_index_`
    - `requests_`
    - `tombstones_`
    - `last_applied_index_`
    - `last_applied_term_`
- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 为 `MetadataStateMachine` 增加最小占位实现
  - `Apply(...)` 返回明确未实现错误，不静默成功
  - `SaveSnapshot(...)` / `LoadSnapshot(...)` 返回明确占位结果，不实现真实 snapshot 序列化
  - `HeadObject(...)` / `ListObjects(...)` 返回最小占位查询结果
- 修改 `tests/metadata_state_machine_test.cpp`
  - 复用现有 `test_metadata_state_machine` target
  - 新增骨架测试 3 个：
    - 空骨架实现 `IStateMachine` 且初始为空
    - `Apply/SaveSnapshot/LoadSnapshot` 返回明确占位结果
    - `HeadObject/ListObjects` 暴露占位查询边界
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 增加 `MetadataStateMachine` 骨架职责说明
  - 明确骨架可以返回明确未实现错误，但不能 silent no-op 成功
  - 明确 `StrongConsistencyMetadataStateMachine` 仍是现有 V1 行为实现，不应在本任务替换默认 wiring

## 3. 骨架边界结果

- `MetadataStateMachine` 已实现 `IStateMachine`
- 已实现接口：
  - `Apply(...)`
  - `SaveSnapshot(...)`
  - `LoadSnapshot(...)`
- 已保留清晰占位字段：
  - `last_applied_index_`
  - `last_applied_term_`
- 已预留 V2 元数据内部表：
  - bucket table
  - object table
  - object index
  - chunk ref index
  - request table
  - tombstone table
- 当前未实现：
  - 完整 apply 状态转换
  - request_id 幂等
  - 真实 snapshot 序列化/反序列化
  - 与 `RaftNode` 默认主路径的接线

## 4. 依赖边界

- `MetadataStateMachine` 仅依赖：
  - `IStateMachine`
  - `raft/common/metadata_command.h`
  - `raft/common/metadata_result.h`
  - `raft/metadata/*` 查询/记录类型
- 本次新增骨架不依赖：
  - `KvStateMachine`
  - `KvService`
  - `raft_kv_client`
  - KV command 类型

## 5. Linux 验证命令

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- `ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

## 6. 为什么选择这些验证命令

- 本次只修改了 `state_machine` 模块源码、对应测试和结构说明，没有修改构建图
- 因此最小闭环是：
  - configure 确认当前 build graph 一致
  - 仅构建 `test_metadata_state_machine`
  - 仅运行 `MetadataStateMachineTest` 过滤
- 不需要扩大到全量构建或全量 CTest

## 7. Linux 结果

- Linux configure
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t012-configure.log`
- Linux build
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t012-build.log`
- Linux CTest
  - 命令：`ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`
  - 结果：`PASS`
  - 统计：`9/9` 通过
  - 说明：
    - 包含新增骨架测试 3 个
    - 同时回归了现有 `StrongConsistencyMetadataStateMachine` 测试 6 个
  - 日志：`tmp/test-logs/t012-ctest.log`

## 8. 未跑全量 CTest 的原因

- 本任务目标是“建立骨架并验证最小编译/最小测试闭环”
- 没有修改默认 wiring、service 行为、proto、持久化格式或全局构建图
- 因此本任务仅运行相关个别测试，未运行全量 CTest

## 9. 风险与限制

- 新增的 `MetadataStateMachine` 目前仍是占位骨架，不可用于正式业务 apply
- `last_applied_index_` / `last_applied_term_` 目前只保留字段和访问器，未接入真实 apply 推进
- `HeadObject` / `ListObjects` 目前只是占位查询边界，不代表最终语义
- 真实 snapshot/recovery 行为仍留给后续任务实现

## 10. 验收结果

- `MetadataStateMachine` 空骨架已建立
- 已实现 `IStateMachine` 接口
- `Apply / SaveSnapshot / LoadSnapshot` 已有最小占位行为
- 内部 metadata 表结构已有清晰占位
- 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现完整 apply / snapshot
- 相关 Linux 构建与个别测试已通过
- 未进入 T013

## 11. 备注

- 仓库当前已有完整的 `StrongConsistencyMetadataStateMachine` 实现；本次没有回退它，只是在同一文件内新增了并行骨架类型
- `tasks.md` 当前已有用户改动，本次未修改 `tasks.md`，避免误覆盖
