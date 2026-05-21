# T013 MetadataStateMachine Bucket apply 最小逻辑

## 1. 结论

- T013 已完成。
- 本次只在 `MetadataStateMachine` 中实现 bucket 级最小 apply：
  - `CreateBucket`
  - `DeleteBucket`
- 本次未实现 object apply、完整 request_id 幂等、真实 snapshot、service 接入或默认 wiring 切换。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.h`
  - 新增 `FindBucket(std::string_view)` 只读辅助接口，供最小测试验证 bucket 状态
- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 新增 `MetadataCommand` 解析辅助路径：
    - 先尝试 `ParseMetadataCommand`
    - 如需要再解析外层 `CommandType::kMetadata` 包装
  - 为 `MetadataStateMachine::Apply(...)` 增加 bucket 命令分支
  - 成功 apply 后更新：
    - `last_applied_index_`
    - `last_applied_term_`
  - 最小记录成功请求到 `requests_`，但不实现完整 request_table 幂等
  - 对不支持命令返回明确错误
- 修改 `tests/metadata_state_machine_test.cpp`
  - 新增 bucket apply 相关测试
  - 调整原 skeleton 占位测试，使其符合“无法解析命令时返回明确错误”的新行为
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 同步说明 `MetadataStateMachine` 当前只承载 bucket 级最小 apply

## 3. Bucket apply 行为

- `CreateBucket`
  - 能识别 `MetadataCommandType::kCreateBucket`
  - bucket 不存在时创建 `BucketRecord`
  - bucket 已存在且未删除时返回明确错误：`state conflict: bucket already exists`
  - bucket 已存在但已删除时，允许用新 payload 覆盖并重新激活
- `DeleteBucket`
  - 能识别 `MetadataCommandType::kDeleteBucket`
  - bucket 不存在时返回明确错误：`not found: bucket does not exist`
  - bucket 已删除时返回明确错误：`state conflict: bucket already deleted`
  - 若 `if_empty=true` 且当前 bucket 下存在未删除对象，占位逻辑会返回：
    - `state conflict: bucket is not empty`
  - 当前 object apply 还未实现，因此该检查主要作为后续扩展预留

## 4. last_applied 边界

- 成功 apply `CreateBucket` / `DeleteBucket` 后：
  - `last_applied_index_` 更新为本次成功 index
  - `last_applied_term_` 更新为 `0`
- 说明：
  - 当前 `IStateMachine::Apply(...)` 只接收 `index` 和 `command_data`
  - 本层尚未获得 term 输入，因此最小实现先在成功 apply 后把 `last_applied_term_` 明确维持为 `0`
  - 失败 apply 不推进 `last_applied_*`

## 5. 不支持命令结果

- 无法解析的输入返回：`failed to parse metadata command`
- `MetadataCommandType::kUnknown` 返回：`unsupported metadata command type: unknown`
- 当前未实现的 object 类命令返回明确错误，例如：
  - `unsupported metadata command type: create_object`

## 6. KV / wiring 边界

- `MetadataStateMachine` 仍不依赖：
  - `KvStateMachine`
  - `KvService`
  - `raft_kv_client`
- 未修改：
  - `CompositeKvMetadataStateMachine`
  - `RaftNode` 默认状态机装配
- 未删除 KV

## 7. Linux 验证命令

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- `ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

## 8. 为什么只跑这些验证

- 本次只修改了 `state_machine` 模块和对应测试
- 没有修改构建图、proto、service 或默认 wiring
- 最小闭环就是：
  - configure
  - 构建 `test_metadata_state_machine`
  - 运行 `MetadataStateMachineTest` 过滤
- 因此本任务未运行全量 CTest

## 9. Linux 结果

- Linux configure
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t013-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t013-build.log`
- Linux CTest
  - 结果：`PASS`
  - 统计：`12/12` 通过
  - 日志：`tmp/test-logs/t013-ctest.log`

## 10. 风险与限制

- 当前只实现 bucket 级最小 apply，不实现：
  - `CreateObject`
  - `CommitObject`
  - `AbortObject`
  - `DeleteObject`
- `requests_` 目前只做最小成功记录，不提供完整幂等语义
- `SaveSnapshot` / `LoadSnapshot` 仍是占位实现
- `last_applied_term_` 目前因接口未传入 term，只能在成功 apply 后明确保持为 `0`

## 11. 验收结果

- `CreateBucket` apply 最小逻辑已实现
- `DeleteBucket` apply 最小逻辑已实现
- `last_applied_index / last_applied_term` 会随成功 apply 更新
- 不支持命令有明确结果
- `MetadataStateMachine` 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现 object apply / snapshot / service
- 相关 Linux 构建与个别测试已通过
- 未进入 T014
