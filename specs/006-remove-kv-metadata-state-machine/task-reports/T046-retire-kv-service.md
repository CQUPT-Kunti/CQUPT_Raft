## T046 完成情况

- 本任务以残留扫描、构建确认和 Linux 验证为主，未修改业务源码。
- 结论：`KvService` 已退役，不再参与当前构建、服务注册和测试执行。

## kv_service_impl / test_kv_service 当前状态

- `modules/raft/service/kv_service_impl.h`：不存在。
- `modules/raft/service/kv_service_impl.cpp`：不存在。
- `tests/test_kv_service.cpp`：不存在。

## 残留扫描结果

- 扫描命令：
  - `rg -n "kv_service_impl|KvService|test_kv_service|PutRequest|GetRequest|DeleteRequest|PutResponse|GetResponse|DeleteResponse|KV fallback" modules tests apps proto CMakeLists.txt tests/CMakeLists.txt`
  - `rg -n "KvService|kv_service_impl|test_kv_service" CMakeLists.txt tests/CMakeLists.txt modules/raft/node/raft_node.cpp modules/raft/service`
  - `rg -n "raft_kv_client|kv\\.proto" CMakeLists.txt tests/CMakeLists.txt modules apps proto`
- 真实结果：
  - 未发现 `kv_service_impl`、`test_kv_service`、`KvService` 的构建引用。
  - 未发现 `raft_kv_client` target 残留。
  - 未发现 `kv.proto` 协议残留。
  - 未发现 `PutRequest` / `GetRequest` / `DeleteRequest` / `PutResponse` / `GetResponse` / `DeleteResponse` 类型残留。
- 非问题命中：
  - `modules/raft/service/AGENTS.md` 中有 “`MetadataService` 不允许回退到 `KvService`” 的规则说明，不是运行时代码。
  - `tests/metadata_concurrency_stress_test.cpp` 中 `ConcurrentDuplicateDeleteRequests...` 是测试名文本，属于 `DeleteRequest` 子串误命中。

## 服务注册与 fallback 确认

- `modules/raft/node/raft_node.cpp` 的 `RaftNode::InitServer()` 仅注册：
  - `RaftServiceImpl`
  - `MetadataServiceImpl`
- 未注册 `KvService`。
- `modules/raft/service/metadata_service_impl.h/.cpp` 未发现 KV fallback、KV RPC 类型或 `KvStateMachine` 依赖。

## CMake / CTest 状态

- `CMakeLists.txt` 的 `raft_core` 仅包含 `raft_service_impl.cpp` 与 `metadata_service_impl.cpp`，不包含 `kv_service_impl.cpp`。
- `tests/CMakeLists.txt` 未引用 `test_kv_service.cpp`。
- 本次未发现需要继续清理的 CMake 或 AGENTS 残留，因此未做源码修改。

## Linux 验证

- 配置：
  - `cmake --preset debug-ninja-low-parallel`
  - 结果：PASS
- 构建：
  - `cmake --build --preset debug-ninja-low-parallel --target raft_demo raft_metadata_client test_metadata_client_scenario test_metadata_failover`
  - 结果：PASS
- CTest：
  - `ctest --test-dir build/linux --output-on-failure -R "(MetadataClientScenarioTest|MetadataFailoverTest|MetadataMainPathTest)"`
  - 结果：PASS（17/17）
  - 说明：本次过滤未匹配到 `MetadataMainPathTest`，实际执行并通过的是 `MetadataClientScenarioTest` 与 `MetadataFailoverTest`。

## 是否可以进入 T047

- 可以进入 `T047`。
- 当前未发现阻塞 `KvService` 退役的真实残留引用。
