# T018 MetadataStateMachine HeadObject / ListObjects 最小查询逻辑

## 1. 结论

- T018 已完成。
- 本次完善了 `MetadataStateMachine` 的 `HeadObject` / `ListObjects` 最小查询逻辑。
- 查询只读取当前内存状态，不进入 Raft Log。
- 查询只暴露 `COMMITTED` 对象，不暴露 `PENDING / DELETED / aborted` 对象。
- 未实现完整 `request_id` 幂等、真实 snapshot、MetadataService 接入或默认 wiring 切换。

## 2. 实际修改

- 修改 `modules/raft/state_machine/metadata_state_machine.cpp`
  - `HeadObject(...)`
    - 新增 bucket 存在性与 deleted 状态检查
    - 只返回 `COMMITTED` 对象
    - 若 `object_index_` 不存在对应条目，返回 `NotFound`
    - 最小支持 `object_id` / `version` 精确匹配过滤
  - `ListObjects(...)`
    - 新增 bucket 存在性与 deleted 状态检查
    - 只收集 `COMMITTED` 对象
    - 跳过 `PENDING / DELETED / tombstone` 对象
    - 最小支持 `prefix` 过滤
    - 按 `object_key` 字典序稳定排序
    - 最小支持 `continuation_token` 的 `start_after` 语义
    - 最小支持 `limit`
    - `next_page_token` 固定为“本页最后一个返回对象的 object_key”
- 修改 `tests/metadata_state_machine_test.cpp`
  - 更新原 skeleton 查询测试，使缺 bucket 的 `ListObjects` 返回 `NotFound`
  - 新增 `HeadObject` committed-only 可见性测试
  - 新增 `ListObjects` prefix / 排序 / limit / continuation_token 测试
- 修改 `modules/raft/state_machine/AGENTS.md`
  - 同步说明 `MetadataStateMachine` 当前已承载 committed-only 的 `HeadObject / ListObjects` 最小查询

## 3. HeadObject 语义固定

- 输入：`bucket + object_key`
- bucket 不存在或已删除：
  - 返回 `NotFound`
- object 不存在或已 `DELETED`：
  - 返回 `NotFound`
- object 为 `PENDING`：
  - 返回 `NotFound`
- object 为 `COMMITTED`：
  - 返回 `ObjectRecord`
- 若提供 `object_id` / `version` 且不匹配：
  - 返回 `NotFound`

## 4. ListObjects 语义固定

- 输入：`bucket`，最小支持：
  - `prefix`
  - `limit`
  - `continuation_token`
- bucket 不存在或已删除：
  - 返回 `NotFound`
- 只列出 `COMMITTED` 对象
- 不列出：
  - `PENDING`
  - `DELETED`
  - aborted 对象
  - tombstone 对象
- 返回顺序：
  - 按 `object_key` 字典序稳定排序
- `continuation_token`：
  - 采用“跳过 `<= token`”的最小 `start_after` 语义
- `next_page_token`：
  - 若发生截断，返回本页最后一个对象的 `object_key`

## 5. 一致性边界

- `Head/List` 都是纯读路径
- 不进入 Raft Log
- 不修改：
  - `last_applied_index_`
  - `last_applied_term_`
  - `objects_`
  - `object_index_`
  - `tombstones_`
- 查询结果与 `object_table / object_index / tombstones_` 保持一致：
  - `COMMITTED + indexed + 非 tombstone` 才可见

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
- 未进入 T019

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
  - 日志：`tmp/test-logs/t018-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t018-build.log`
- Linux CTest
  - 结果：`PASS`
  - 统计：`22/22` 通过
  - 日志：`tmp/test-logs/t018-ctest.log`

## 10. 风险与限制

- 当前只实现最小查询逻辑
- `include_deleted` 仍未提供完整对外语义，当前 committed-only 查询会忽略 deleted 可见性扩展
- `requests_` 目前只做最小成功记录，不提供完整幂等判定
- `SaveSnapshot / LoadSnapshot` 仍是占位实现
- 当前只验证了 Linux，Windows 留待后续 Windows 环境补测

## 11. 验收结果

- `HeadObject` 最小查询逻辑已实现
- `ListObjects` 最小查询逻辑已实现
- `Head/List` 不进入 Raft Log
- `Head/List` 不修改状态机 apply 边界
- `COMMITTED` 对象可查询
- `PENDING / DELETED / aborted` 对象不会被错误暴露
- `ListObjects` 支持最小 `prefix` 过滤和稳定顺序
- `object_table / object_index / tombstones_` 查询语义一致
- `MetadataStateMachine` 不依赖 KV
- 未修改 `RaftNode` 默认 wiring
- 未删除 KV
- 未实现 `snapshot / service`
- 相关 Linux 构建与个别测试已通过
