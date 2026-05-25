# T011 service/client proto 依赖边界收敛

## 1. 结论

- T011 已完成，范围限定为 proto target/link 边界收敛、受影响 service/client/test 的最小依赖确认，以及相关 `AGENTS.md` 同步。
- 本次没有修改业务逻辑、协议语义、默认 `RaftNode` wiring、`KvService`/`raft_kv_client` 存废状态，也没有进入 T012。
- 验收结论：
  - `RaftServiceImpl` 继续只依赖 `raft.proto` 生成头
  - metadata 相关代码只依赖 `metadata.proto` / `common.proto` 生成头
  - `KvService` / `raft_kv_client` 只依赖 `kv.proto` / `common.proto` 生成头
  - 根 CMake 已从“单一聚合 `raft_proto`”收敛为清晰的 `common_proto` / `raft_proto` / `metadata_proto` / `kv_proto`

## 2. 实际修改内容

- 修改根 `CMakeLists.txt`
  - 新增 `common_proto`
  - 保留 `raft_proto`，其职责收敛为仅承载 `raft.proto`
  - 新增 `metadata_proto`
  - 新增 `kv_proto`
  - 调整 `raft_core`、`raft_kv_client`、`raft_metadata_client` 的 proto link 边界
- 修改 `tests/CMakeLists.txt`
  - `test_metadata_failover` 显式链接 `metadata_proto`
  - `test_metadata_client_scenario` 从 `raft_proto` 改为显式链接 `metadata_proto`
  - `test_kv_service` 显式链接 `kv_proto`
- 修改边界说明
  - `proto/AGENTS.md`
  - `modules/raft/service/AGENTS.md`
  - `apps/AGENTS.md`

## 3. CMake target 边界结果

- `common_proto`
  - 只承载 `common.pb.*`
  - 供 `metadata_proto`、`kv_proto` 复用
- `raft_proto`
  - 只承载 `raft.pb.*`、`raft.grpc.pb.*`
  - 不再混入 metadata/KV 生成代码
- `metadata_proto`
  - 承载 `metadata.pb.*`、`metadata.grpc.pb.*`
  - 通过 `common_proto` 复用公共消息
- `kv_proto`
  - 承载 `kv.pb.*`、`kv.grpc.pb.*`
  - 通过 `common_proto` 复用公共消息

## 4. service/client/test 依赖结果

- `modules/raft/service/raft_service_impl.h`
  - 仅 include `raft.grpc.pb.h`
- `modules/raft/service/metadata_service_impl.h`
  - 仅 include `metadata.grpc.pb.h`
- `modules/raft/service/kv_service_impl.h`
  - 仅 include `kv.grpc.pb.h`
- `apps/raft_metadata_client.cpp`
  - 仅 include `metadata.grpc.pb.h`
- `apps/raft_kv_client.cpp`
  - 仅 include `kv.grpc.pb.h`
- `tests/metadata_failover_test.cpp`
  - 仅 include `metadata.grpc.pb.h`
- `tests/metadata_client_scenario_test.cpp`
  - 仅 include `metadata.grpc.pb.h`
- `tests/test_kv_service.cpp`
  - 仅 include `kv.grpc.pb.h`
- 静态检查结果表明 metadata 相关文件未复用 `Put/Get/Delete` 消息

## 5. Linux 验证命令

- `cmake --preset debug-ninja-low-parallel`
- `cmake --build --preset debug-ninja-low-parallel --target common_proto raft_proto metadata_proto kv_proto raft_demo raft_kv_client raft_metadata_client test_kv_service test_metadata_client_scenario test_metadata_failover`

## 6. 为什么选择这些命令

- 本次改动触达了根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，因此必须重新跑 configure
- 改动内容是 target/link 边界，而不是业务逻辑，因此最小必要闭环是：
  - 重新生成/解析 CMake target 图
  - 构建四个 proto target
  - 构建直接依赖这些 target 的 demo/client/test 可执行文件
- 不需要默认扩大到全量 CTest

## 7. Linux 结果

- Linux configure
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t011-configure.log`
- Linux build
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t011-build.log`
  - 构建日志确认完成了以下目标的链接或生成：
    - `libcommon_proto.a`
    - `libraft_proto.a`
    - `libmetadata_proto.a`
    - `libkv_proto.a`
    - `raft_demo`
    - `raft_kv_client`
    - `raft_metadata_client`
    - `tests/test_kv_service`
    - `tests/test_metadata_client_scenario`
    - `tests/test_metadata_failover`

## 8. CTest 结果

- 本任务未运行 CTest。
- 原因：
  - 本次没有修改 service/client 行为逻辑
  - 本次验证目标是“生成头与链接边界是否正确收敛”
  - 受影响 target 已完成 configure + compile + link 验证，足以覆盖本次改动范围
- 说明：本任务仅运行相关个别构建验证，未运行全量 CTest。

## 9. Windows 结果

- 当前任务仅在 Linux 环境验证，Windows 留待后续 Windows 环境补测。

## 10. KV 主路径状态

- 未删除 `KvStateMachine`
- 未删除 `KvService` C++ 实现
- 未删除 `raft_kv_client`
- 未删除 `kv.proto`
- 未修改 `RaftNode` 默认 wiring
- 未实现 `MetadataService` 业务逻辑
- 未实现 `MetadataStateMachine` apply
- 未进入 T012

## 11. 备注

- `specs/006-remove-kv-metadata-state-machine/tasks.md` 当前已有用户改动，且其中 `T011` 与本次执行定义不一致；为避免误标错误任务，本次未修改 `tasks.md`
