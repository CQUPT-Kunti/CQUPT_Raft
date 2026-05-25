# T015 MetadataStateMachine CommitObject apply 最小逻辑

## 1. 结论

- T015 已完成。
- 本次在 `MetadataStateMachine` 中实现了 `CommitObject` 的最小 apply 逻辑。
- 本次只处理 `PENDING -> COMMITTED` 状态转换。
- 未实现 `AbortObject / DeleteObject`、完整 `request_id` 幂等、真实 snapshot、MetadataService 接入或默认 wiring 切换。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.h`
  - 新增 `FindChunkRefs(std::string_view bucket, std::string_view object_key)` 只读辅助接口
- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 扩展 `MakeAppliedRequestRecord(...)`，补齐 object 类命令到 `MetadataRequestType` 的最小映射
  - 在 `MetadataStateMachine::Apply(...)` 中增加 `CommitObject` 分支
  - 成功 commit 后：
    - 将对象状态从 `PENDING` 改为 `COMMITTED`
    - 写入 `size`
    - 写入 `etag`
    - 写入 `chunks`
    - 写入 `commit_time`
    - 同步维护 `chunk_ref_index_`
    - 记录最小 `requests_`
    - 更新 `last_applied_index_`
    - 按当前占位语义把 `last_applied_term_` 维持为 `0`
- 修改 `tests/metadata_state_machine_test.cpp`
  - 新增 `CommitObject` 成功路径测试
  - 新增 bucket 不存在、bucket 已删除、object 不存在、`object_id` 不匹配、重复 commit 等错误路径测试
  - 将原“不支持命令”测试更新为仍未实现的 `AbortObject`
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 同步说明 `MetadataStateMachine` 当前已承载 `CommitObject` 的最小 `COMMITTED` apply

## 3. CommitObject 最小行为

- 能识别 `MetadataCommandType::kCommitObject`
- 要求 bucket 存在且未删除
- 要求 object 已存在
- 要求 `object_id` 匹配
- 只允许提交 `PENDING` 对象
- 成功后对象状态变为 `COMMITTED`
- 成功后保存：
  - `size`
  - `etag`
  - `chunks`
  - `commit_time`
- 同步把 chunk 列表写入 `chunk_ref_index_`

## 4. 明确错误结果

- bucket 不存在：
  - `not found: bucket does not exist`
- bucket 已删除：
  - `state conflict: bucket is deleted`
- object 不存在：
  - `not found: object does not exist`
- `object_id` 不匹配：
  - `state conflict: object_id mismatch`
- 对象已 `COMMITTED`：
  - `state conflict: object already committed`
- 其他非 `PENDING` 状态：
  - `state conflict: object is not pending`
- 当前仍未实现的 object 命令继续返回：
  - `unsupported metadata command type: ...`

## 5. last_applied 语义

- 成功 apply `CommitObject` 后：
  - `last_applied_index_` 更新为本次成功 index
  - `last_applied_term_` 维持为 `0`
- 失败 apply 不推进 `last_applied_*`
- 当前 `IStateMachine::Apply(...)` 仍只接收 `index + command_data`
- 本任务不扩展 term 输入，因此保持 T012/T013/T014 的占位语义

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
- 未进入 T016

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
  - 日志：`tmp/test-logs/t015-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t015-build.log`
- Linux CTest
  - 结果：`PASS`
  - 统计：`16/16` 通过
  - 日志：`tmp/test-logs/t015-ctest.log`

## 10. 风险与限制

- 当前只实现 `CommitObject` 最小 apply，不实现：
  - `AbortObject`
  - `DeleteObject`
- `HeadObject / ListObjects` 本次未补完整查询语义
- `requests_` 目前只做最小成功记录，不提供完整幂等判定
- `SaveSnapshot / LoadSnapshot` 仍是占位实现
- 当前只验证了 Linux，Windows 留待后续 Windows 环境补测

## 11. 验收结果

- `CommitObject` apply 最小逻辑已实现
- `PENDING ObjectRecord` 能成功转为 `COMMITTED`
- `size / etag / ChunkRef` 列表已保存到 `ObjectRecord`
- `chunk_ref_index_` 已同步维护
- bucket 不存在、bucket 已删除、object 不存在、`object_id` 不匹配、object 非 `PENDING` 都有明确错误
- `last_applied_index / last_applied_term` 按当前语义更新
- `MetadataStateMachine` 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现 `AbortObject / DeleteObject / snapshot / service`
- 相关 Linux 构建与个别测试已通过
