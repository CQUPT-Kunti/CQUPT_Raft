# T025 删除 KV service/client 过渡入口

## 结论

- T025 已完成。
- `KvService` 不再作为服务入口绑定。
- `raft_kv_client` 不再作为主构建入口。
- `test_kv_service` 已删除。
- metadata 主路径不再依赖 KV service/client/proto。
- `KvStateMachine` 仍保留，等待后续专门删除任务。

## 实际删除/迁移

- 删除 KV 入口文件：
  - `modules/raft/service/kv_service_impl.h`
  - `modules/raft/service/kv_service_impl.cpp`
  - `apps/raft_kv_client.cpp`
  - `proto/kv.proto`
  - `tests/test_kv_service.cpp`
- 更新 `modules/raft/node/raft_node.h/.cpp`
  - 去掉 `KvServiceImpl` 前置声明、friend、成员指针
  - `RaftNode::InitServer()` 不再创建/注册 `KvServiceImpl`
  - 默认 gRPC 服务绑定现在只剩：
    - `RaftServiceImpl`
    - `MetadataServiceImpl`
- 更新根 `CMakeLists.txt`
  - 移除 `kv.proto` 生成
  - 移除 `kv_proto` target
  - `raft_core` 不再编译 `kv_service_impl.cpp`
  - `raft_core` 不再链接 `kv_proto`
  - 移除 `raft_kv_client` 可执行 target
- 更新 `tests/CMakeLists.txt`
  - 移除 `test_kv_service` target
- 更新 `test.sh`
  - 移除 `kv-service` 分组、帮助文本和 `all` 主扫入口
  - 保留 `KvStateMachineTest` 所在 `unit` 分组，不影响后续旧状态机回归
- 更新 AGENTS 文档：
  - `modules/raft/service/AGENTS.md`
  - `proto/AGENTS.md`
  - `apps/AGENTS.md`
  - 同步去掉 KV service/client/proto 入口说明，保留 metadata 主路径说明

## 仍保留的 KV 过渡残留

- `KvStateMachine`
- 旧 KV `CommandType` / `Command` 编解码
- `tests/test_state_machine.cpp` 中的 `KvStateMachineTest`
- 这些残留仍用于后续 Raft 核心回归迁移，本任务未删除

## 默认 wiring 结果

- `RaftNode` 默认状态机 wiring 未回退到 KV/Composite。
- `RaftNode` 默认服务入口不再包含 `KvService`。
- `MetadataService` 主路径未引入 KV fallback。

## Linux 验证

- 选择原因
  - 本次修改影响服务绑定、主构建入口、proto generation 和 metadata 主路径构建图
  - 因此执行 configure + 受影响 target build + metadata 主路径相关 CTest 过滤
  - 未扩大到全量 CTest
- 实际命令
  - `cmake --preset debug-ninja-low-parallel`
  - `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client test_metadata_client_scenario test_metadata_failover test_metadata_state_machine`
  - `CTEST_PARALLEL_LEVEL=1 ctest --test-dir build/linux --output-on-failure -R "^(MetadataStateMachineTest|MetadataFailoverTest|MetadataClientScenarioTest)\."`
- 结果
  - configure：PASS
  - build：PASS
  - CTest：PASS
  - `MetadataStateMachineTest + MetadataFailoverTest + MetadataClientScenarioTest` 合计 `40/40` 通过

## 日志

- `tmp/test-logs/t025-configure.log`
- `tmp/test-logs/t025-build.log`
- `tmp/test-logs/t025-ctest.log`

## 风险与范围

- 本任务仅运行相关个别测试/构建验证，未运行全量 CTest。
- 当前任务仅在 Linux 环境验证，Windows 留待后续 Windows 环境补测。
- 未删除 `KvStateMachine`。
- 未修改 `RaftNode` 默认 metadata 状态机 wiring。
- 未破坏 `MetadataService` 主路径。
- 未进入 T026。
