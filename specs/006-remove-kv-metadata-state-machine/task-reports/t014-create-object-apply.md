# T014 MetadataStateMachine CreateObject apply 最小逻辑

## 1. 结论

- T014 已完成。
- 本次在 `MetadataStateMachine` 中实现了 `CreateObject` 的最小 apply 逻辑。
- 本次只创建 `PENDING` `ObjectRecord`，未实现 `CommitObject / AbortObject / DeleteObject`。
- 未实现完整 `request_id` 幂等、真实 snapshot、MetadataService 接入或默认 wiring 切换。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.h`
  - 新增只读辅助接口：
    - `FindObject(std::string_view bucket, std::string_view object_key)`
    - `FindIndexedObjectId(std::string_view bucket, std::string_view object_key)`
- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 新增 `bucket + object_key` 组合键 helper，用于 `objects_` / `object_index_`
  - 在 `MetadataStateMachine::Apply(...)` 中增加 `CreateObject` 分支
  - `CreateObject` 成功后：
    - 写入 `objects_`
    - 写入 `object_index_`
    - 记录最小 `requests_`
    - 更新 `last_applied_index_`
    - 按当前占位语义把 `last_applied_term_` 维持为 `0`
- 修改 `tests/metadata_state_machine_test.cpp`
  - 新增 `CreateObject` 成功路径测试
  - 新增缺 bucket、bucket 已删除、重复 object_key 冲突测试
  - 调整原“不支持命令”测试，使其改为仍未实现的 `CommitObject`
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 同步说明 `MetadataStateMachine` 当前已承载 `CreateObject` 的最小 `PENDING` apply

## 3. CreateObject 最小行为

- 能识别 `MetadataCommandType::kCreateObject`
- 成功时创建 `PENDING` `ObjectRecord`
- 最小保留并验证以下字段：
  - `bucket`
  - `object_key`
  - `object_id`
  - `version`
  - `size`
  - `state`
  - `create_time`
- 维护 `object_index_`：
  - `bucket/object_key -> object_id`
- 成功 apply 后记录最小请求结果到 `requests_`

## 4. 明确错误结果

- bucket 不存在：
  - `not found: bucket does not exist`
- bucket 已删除：
  - `state conflict: bucket is deleted`
- 同一 bucket 下 `object_key` 已存在且未删除：
  - `state conflict: object already exists`
- 仍未实现的 object 命令：
  - 继续返回 `unsupported metadata command type: ...`

## 5. last_applied 语义

- 成功 apply `CreateObject` 后：
  - `last_applied_index_` 更新为本次成功 index
  - `last_applied_term_` 维持为 `0`
- 失败 apply 不推进 `last_applied_*`
- 说明：
  - 当前 `IStateMachine::Apply(...)` 仍只接收 `index + command_data`
  - 本任务不扩展 term 输入，因此保持 T012/T013 的占位语义

## 6. 边界确认

- `MetadataStateMachine` 未依赖：
  - `KVCommand`
  - `KvStateMachine`
  - `KvService`
  - `raft_kv_client`
- 未修改：
  - `CompositeKvMetadataStateMachine`
  - `RaftNode` 默认状态机装配
- 未删除 KV
- 未进入 T015

## 7. Linux 验证命令

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- `ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

## 8. 为什么只跑这些验证

- 本次只改动 `state_machine` 模块源码、对应测试和模块说明
- 没有修改 proto、service、默认 wiring 或全局构建图
- 最小可证明闭环就是：
  - configure
  - 构建 `test_metadata_state_machine`
  - 运行 `MetadataStateMachineTest` 定向过滤
- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest

## 9. Linux 结果

- Linux configure
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t014-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t014-build.log`
- Linux CTest
  - 结果：`PASS`
  - 统计：`14/14` 通过
  - 日志：`tmp/test-logs/t014-ctest.log`

## 10. 风险与限制

- 当前只实现 `CreateObject` 最小 apply，不实现：
  - `CommitObject`
  - `AbortObject`
  - `DeleteObject`
- `HeadObject / ListObjects` 本次未补完整查询语义
- `requests_` 目前只做最小成功记录，不提供完整幂等判定
- `SaveSnapshot / LoadSnapshot` 仍是占位实现

## 11. 验收结果

- `CreateObject` apply 最小逻辑已实现
- 成功 `CreateObject` 会生成 `PENDING ObjectRecord`
- `object_index_` 已建立 `bucket/object_key -> object_id` 映射
- bucket 不存在、bucket 已删除、object 已存在等错误有明确结果
- `last_applied_index / last_applied_term` 按当前语义更新
- `MetadataStateMachine` 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现 `CommitObject / AbortObject / DeleteObject / snapshot / service`
- 相关 Linux 构建与个别测试已通过
