# T017 MetadataStateMachine DeleteObject apply 最小逻辑

## 1. 结论

- T017 已完成。
- 本次在 `MetadataStateMachine` 中实现了 `DeleteObject` 的最小 apply 逻辑。
- 本次只处理删除 `COMMITTED` 对象。
- 未实现完整 `request_id` 幂等、真实 snapshot、MetadataService 接入或默认 wiring 切换。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - 在 `MetadataStateMachine::Apply(...)` 中增加 `DeleteObject` 分支
  - 成功 delete 后：
    - 将对象状态置为 `DELETED`
    - 写入 `delete_time`
    - 从 `object_index_` 移除该对象
    - 从 `chunk_ref_index_` 移除该对象
    - 写入 `tombstones_`
    - 记录最小 `requests_`
    - 更新 `last_applied_index_`
    - 按当前占位语义把 `last_applied_term_` 维持为 `0`
- 修改 `tests/metadata_state_machine_test.cpp`
  - 新增 `DeleteObject` 成功路径测试
  - 新增 bucket 不存在、bucket 已删除、object 不存在、`object_id` 不匹配、对象非 `COMMITTED`、对象已删除等错误路径测试
  - 将原 bucket/unsupported 组合测试收敛为“非法 delete payload 返回明确错误”
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 同步说明 `MetadataStateMachine` 当前已承载 `DeleteObject` 的最小删除态 apply

## 3. Delete 语义固定

- 能识别 `MetadataCommandType::kDeleteObject`
- 要求 bucket 存在且未删除
- 要求 object 已存在
- 要求 `object_id` 匹配
- 只允许删除 `COMMITTED` 对象
- 对 `PENDING` 对象：
  - 返回 `state conflict: object is not committed`
  - 明确要求继续走 `AbortObject`
- 成功 delete 后的固定语义：
  - `objects_` 中保留该对象记录
  - `ObjectRecord.state = DELETED`
  - `object_index_` 移除该对象
  - `chunk_ref_index_` 移除该对象
  - `tombstones_` 记录删除事实

## 4. 明确错误结果

- bucket 不存在：
  - `not found: bucket does not exist`
- bucket 已删除：
  - `state conflict: bucket is deleted`
- object 不存在：
  - `not found: object does not exist`
- `object_id` 不匹配：
  - `state conflict: object_id mismatch`
- 对象不是 `COMMITTED`：
  - `state conflict: object is not committed`
- 对象已删除：
  - `state conflict: object already deleted`
- 非法 delete payload：
  - `invalid metadata command: delete_object command missing object_id`

## 5. 可见性结果

- 删除后的对象不会继续作为有效对象暴露
- `HeadObject(...)` 对 `DELETED` 对象返回 `NotFound`
- `ListObjects(...)` 当前仍为空占位返回，因此删除对象不会被列出

## 6. last_applied 语义

- 成功 apply `DeleteObject` 后：
  - `last_applied_index_` 更新为本次成功 index
  - `last_applied_term_` 维持为 `0`
- 失败 apply 不推进 `last_applied_*`
- 当前 `IStateMachine::Apply(...)` 仍只接收 `index + command_data`
- 本任务不扩展 term 输入，因此保持 T012-T016 的占位语义

## 7. 边界确认

- `MetadataStateMachine` 未依赖：
  - `KVCommand`
  - `KvStateMachine`
  - `KvService`
  - `raft_kv_client`
- 未修改：
  - `CompositeKvMetadataStateMachine`
  - `RaftNode` 默认状态机装配
- 未删除 KV
- 未进入 T018

## 8. Linux 验证命令

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target test_metadata_state_machine`
- `ctest --test-dir build/linux --output-on-failure -R "^MetadataStateMachineTest\\."`

## 9. 为什么只跑这些验证

- 本次只改动 `state_machine` 模块源码、对应测试和模块说明
- 没有修改 proto、service、默认 wiring 或全局构建图
- 最小可证明闭环就是：
  - configure
  - 构建 `test_metadata_state_machine`
  - 运行 `MetadataStateMachineTest` 定向过滤
- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest

## 10. Linux 结果

- Linux configure
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t017-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t017-build.log`
- Linux CTest
  - 结果：`PASS`
  - 统计：`20/20` 通过
  - 日志：`tmp/test-logs/t017-ctest.log`

## 11. 风险与限制

- 当前只实现 `DeleteObject` 最小 apply
- `HeadObject / ListObjects` 仍不是完整业务查询实现，只补了 deleted 对象隐藏边界
- `requests_` 目前只做最小成功记录，不提供完整幂等判定
- `SaveSnapshot / LoadSnapshot` 仍是占位实现
- 当前只验证了 Linux，Windows 留待后续 Windows 环境补测

## 12. 验收结果

- `DeleteObject` apply 最小逻辑已实现
- `COMMITTED ObjectRecord` 能被成功删除
- 删除后对象状态为 `DELETED`
- 删除后 `object_index_ / chunk_ref_index_` 已清理
- 删除后 `tombstones_` 已记录删除事实
- `HeadObject` 不再暴露已删除对象
- bucket 不存在、bucket 已删除、object 不存在、`object_id` 不匹配、对象非 `COMMITTED`、对象已 `DELETED` 都有明确错误
- `last_applied_index / last_applied_term` 按当前语义更新
- `MetadataStateMachine` 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现 `snapshot / service`
- 相关 Linux 构建与个别测试已通过
