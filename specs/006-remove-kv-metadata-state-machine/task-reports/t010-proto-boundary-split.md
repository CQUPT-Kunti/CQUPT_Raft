# T010 proto 边界拆分

## 1. T010 结论

- T010 已完成，范围限定为 proto 边界拆分、proto generation target 调整、直接 include 生成头的 service/client/test 收口，以及相关 `AGENTS.md` 更新。
- 本次把原先混在 `proto/raft.proto` 里的共识 RPC、metadata RPC、公共消息、遗留 KV RPC 拆开：
  - `proto/raft.proto`：只保留 `RaftService` 和 Raft 共识消息
  - `proto/metadata.proto`：只保留 `MetadataService` 和 metadata RPC
  - `proto/common.proto`：承载 `Status/Health/Metrics`、metadata 状态/摘要等公共消息
  - `proto/kv.proto`：过渡期承载 `KvService` 和 `Put/Get/Delete`，避免当前仓库在未进入 KV 删除任务前失编
- 结果满足“`KvService`/KV message 不再与 `RaftService` 共处同一主 proto 边界”，同时没有修改业务逻辑、协议语义、持久化格式和默认 `RaftNode` wiring。

## 2. 新增/修改文件

- 新增 `proto/common.proto`
- 新增 `proto/metadata.proto`
- 新增 `proto/kv.proto`
- 修改 `proto/raft.proto`
- 修改 `CMakeLists.txt`
- 修改 `modules/raft/service/kv_service_impl.h`
- 修改 `modules/raft/service/metadata_service_impl.h`
- 修改 `apps/raft_kv_client.cpp`
- 修改 `apps/raft_metadata_client.cpp`
- 修改 `tests/test_kv_service.cpp`
- 修改 `tests/metadata_failover_test.cpp`
- 修改 `tests/metadata_client_scenario_test.cpp`
- 修改 `proto/AGENTS.md`
- 修改 `modules/raft/service/AGENTS.md`
- 修改 `apps/AGENTS.md`
- 新增报告 `specs/006-remove-kv-metadata-state-machine/task-reports/t010-proto-boundary-split.md`

## 3. proto 边界结果

- `raft.proto`
  - 仅保留 `RaftService`、`Vote*`、`AppendEntries*`、`InstallSnapshot*`、`LogEntry`
- `metadata.proto`
  - 仅保留 `MetadataService`
  - 仅保留 `Create/Commit/Delete/Head/ListMetadataRecord*` 请求响应
  - 通过 `import "common.proto"` 使用 metadata 公共消息
- `common.proto`
  - 承载 `StatusRequest/Response`、`Health*`、`Metrics*`
  - 承载 `PeerReplicationProgress`、`RpcMetric`、`MetricsSnapshot`
  - 承载 `MetadataStatusCode`、`MetadataRecordState`、`MetadataManifest`、`MetadataRecord`、`MetadataResponseSummary`
- `kv.proto`
  - 过渡期承载 `KvService`
  - 承载 `KvStatusCode`、`Put/Get/Delete*`
  - 通过 `import "common.proto"` 复用状态/指标公共消息

## 4. CMake proto generation 更新

- 根 `CMakeLists.txt` 的单文件 `PROTO_FILE` 已改为多文件：
  - `PROTO_FILES = common.proto + raft.proto + metadata.proto + kv.proto`
  - `GRPC_PROTO_FILES = raft.proto + metadata.proto + kv.proto`
- `raft_proto` target 名称保持不变
- Linux 生成产物已确认包含：
  - `build/linux/generated/common.pb.*`
  - `build/linux/generated/raft.pb.*`
  - `build/linux/generated/raft.grpc.pb.*`
  - `build/linux/generated/metadata.pb.*`
  - `build/linux/generated/metadata.grpc.pb.*`
  - `build/linux/generated/kv.pb.*`
  - `build/linux/generated/kv.grpc.pb.*`

## 5. include 边界收口

- `raft_service_impl` 继续使用 `raft.grpc.pb.h`
- `metadata_service_impl` 改为使用 `metadata.grpc.pb.h`
- `raft_metadata_client` 改为使用 `metadata.grpc.pb.h`
- `kv_service_impl` 改为使用 `kv.grpc.pb.h`
- `raft_kv_client`、`test_kv_service` 改为使用 `kv.grpc.pb.h`
- metadata 相关测试改为使用 `metadata.grpc.pb.h`

## 6. AGENTS.md 更新

- `proto/AGENTS.md`
  - 补充 `raft.proto` / `metadata.proto` / `common.proto` / `kv.proto` 的职责边界
  - 说明 `kv.proto` 仅为过渡期残留 KV RPC 隔离面
- `modules/raft/service/AGENTS.md`
  - 补充 `metadata_service_impl.*`
  - 明确 service 层应分别依赖对应 proto 生成头，不再通过 `raft.proto` 混装
- `apps/AGENTS.md`
  - 补充 `raft_metadata_client.cpp`
  - 明确两个 client 分别消费各自 proto 生成头

## 7. Linux 结果

- Linux configure
  - 命令：`cmake --preset debug-ninja-low-parallel`
  - 结果：`PASS`
  - 日志：`tmp/test-logs/t010-configure.log`
- Linux build
  - 命令：`cmake --build --preset debug-ninja-low-parallel --target raft_proto raft_demo raft_kv_client raft_metadata_client test_kv_service test_metadata_client_scenario test_metadata_failover`
  - 结果：`PASS`
  - 关键证明：
    - 重新生成了 `common/raft/metadata/kv` 四组 protobuf 代码
    - 重新链接了 `raft_proto`、`raft_core`、`raft_demo`
    - 重新链接了 `raft_kv_client`、`raft_metadata_client`
    - 重新链接了 `test_kv_service`、`test_metadata_failover`、`test_metadata_client_scenario`
  - 日志：`tmp/test-logs/t010-build.log`

## 8. Windows 结果

- 当前任务仅在 Linux 环境验证，Windows 留待后续 Windows 环境补测。

## 9. CTest 结果

- 本任务仅运行相关个别构建验证，未运行全量 CTest。
- 本次未执行任何测试用例；原因是 T010 目标是 proto/CMake/生成代码边界拆分，当前只要求验证 Linux configure/build 和受影响 target 可成功生成、编译、链接。

## 10. KV removal status

- 未删除 `KvStateMachine`
- 未修改 `RaftNode` 默认 wiring
- 未实现 `MetadataService` 新业务逻辑
- 未实现 `MetadataStateMachine` apply
- 未把 `MetadataCommand` 接入 Raft log
- 未实现 `DataNode`
- 未进入 T011

## 11. 影响与后续风险

- 当前 `KvService` 仍存在，但已被隔离到 `kv.proto`，不再和 `RaftService` 同处主 proto 边界
- `Status/Health/Metrics` 目前仍通过 `KvService` 暴露；后续若要落到独立管理面服务，需要后续任务继续推进
- 为避免误把 `tasks.md` 中当前定义的另一个 `T010` 标记完成，本次未修改 `tasks.md`
- 本次未运行全量 CTest；更大范围回归需留给后续任务或专门验证任务执行
