# T015 Report

## T015 任务目标

实现 `MetadataService` 的 create / commit / head / list service adapter，并打通 metadata command 到 Raft proposal 的最小合法路径。

## 上一轮为什么 BLOCKED

- `RaftNode` 公开接口只有 `Propose(const Command&)`，而 `Command` 只支持 `kSet / kDelete`。
- service 层没有合法的 metadata proposal 入口，也没有 committed-only metadata 查询入口。
- 如果强行复用 KV `Put/Get/Delete` 语义，会违反任务边界。

## 本次如何解除阻塞

- 在 `Command` 层新增独立的 `CommandType::kMetadata`，使用独立的 `META|payload_size|payload` 封装 metadata payload，不影响现有 `SET|key|value` / `DEL|key|`。
- 在 `metadata_command.h` 暴露 codec 声明，让 service / state_machine 可直接复用既有 codec。
- 在 `StrongConsistencyMetadataStateMachine::Apply` 中同时兼容：
  - 直接传入的原始 `META1` metadata command；
  - 通过 `CommandType::kMetadata` 包装后进入 Raft log 的 metadata command。
- 在 `RaftNode` 中新增最小 metadata proposal/query 入口：
  - `ProposeMetadata(...)`
  - `GetMetadataStateMachine()`
- 新增 `MetadataServiceImpl`，完成 create / commit / head / list adapter。

## 修改了哪些文件

- `modules/raft/common/command.h`
- `modules/raft/common/command.cpp`
- `modules/raft/common/metadata_command.h`
- `modules/raft/state_machine/metadata_state_machine.cpp`
- `modules/raft/service/metadata_service_impl.h`
- `modules/raft/service/metadata_service_impl.cpp`
- `modules/raft/node/raft_node.h`
- `modules/raft/node/raft_node.cpp`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`

## 每个文件大概改了什么

- `modules/raft/common/command.h/.cpp`
  - 新增 `CommandType::kMetadata` 和 `metadata_payload`。
  - 新增 metadata 命令的独立序列化格式，保持 `kSet / kDelete` 兼容。
- `modules/raft/common/metadata_command.h`
  - 补充 metadata codec 的函数声明。
- `modules/raft/state_machine/metadata_state_machine.cpp`
  - 支持从 `CommandType::kMetadata` 包装命令中解出 `META1` payload。
  - 保持原有直接 `META1` 路径兼容，未改状态转换语义。
- `modules/raft/service/metadata_service_impl.h/.cpp`
  - 新增 `MetadataServiceImpl`。
  - 实现 `CreateMetadataRecord`、`CommitMetadataRecord`、`HeadMetadataRecord`、`ListMetadataRecords` adapter。
  - `DeleteMetadataRecord` 当前返回明确未实现错误，留给后续任务。
- `modules/raft/node/raft_node.h/.cpp`
  - 新增 `ProposeMetadata(...)`。
  - 新增 `GetMetadataStateMachine()` 查询入口。
  - 不影响现有 KV `Propose(...)` 行为。
- `CMakeLists.txt`
  - 将 metadata command / state machine / service 源文件接入 `raft_core`。
- `tests/CMakeLists.txt`
  - 移除 metadata 测试对这些源文件的重复显式编入，避免与 `raft_core` 重复链接。

## 是否执行了验证

- 已执行：
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target raft_demo test_metadata_command test_metadata_state_machine`
  - `ctest --test-dir build/linux --output-on-failure -R 'Metadata(Command|StateMachine)Test'`
- 结果：
  - 配置通过
  - 构建通过
  - metadata 相关测试 15/15 通过

## 当前风险或后续事项

- `MetadataServiceImpl` 已完成编译，但还没有在 `apps/main.cpp` 注册；这属于后续 T017。
- `DeleteMetadataRecord` adapter 仍为明确未实现返回；真正 delete 适配留给 T022。
- `CommitMetadataRecordRequest.expected_create_request_id` 当前协议字段尚未进入公共 command/state machine 模型校验，当前 adapter 未使用该字段。

## 建议 commit message

```text
feat(metadata): 打通 metadata proposal 路径并完成 MetadataService adapter
```
