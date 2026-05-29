# T030 Storage Node Proto Codegen

## 修改文件

- `CMakeLists.txt`
- `specs/007-storage-node-data-plane/task-reports/common-risk-notes.md`
- `specs/007-storage-node-data-plane/tasks.md`

## 做了什么

- 将 `proto/storage_node.proto` 接入现有 protobuf / gRPC codegen 列表
- 新增独立 `storage_node_proto` target，生成并承载 `storage_node.pb.*` 与 `storage_node.grpc.pb.*`
- 保持 `raft_proto`、`metadata_proto` 与新 storage target 的边界清晰，不让 `raft_proto` 反向依赖 storage node 生成代码

## 新增/修改的 CMake target 或 proto 列表

- `PROTO_FILES`
  - 新增 `${STORAGE_NODE_PROTO_SCHEMA}`
- `GRPC_PROTO_FILES`
  - 新增 `${STORAGE_NODE_PROTO_SCHEMA}`
- `PROTO_SRCS` / `PROTO_HDRS`
  - 新增 `storage_node.pb.cc`
  - 新增 `storage_node.pb.h`
- `GRPC_SRCS` / `GRPC_HDRS`
  - 新增 `storage_node.grpc.pb.cc`
  - 新增 `storage_node.grpc.pb.h`
- 新增 target：`storage_node_proto`
  - include：`${GENERATED_DIR}`
  - link：`protobuf::libprotobuf`、`gRPC::grpc++`
  - Windows 下补 `Ws2_32`

## storage_node.proto 生成产物边界

- 生成产物仍落在现有 `${CMAKE_CURRENT_BINARY_DIR}/generated`
- `storage_node_proto` 只承载 storage node proto / gRPC 生成代码
- `raft_proto` 继续只承载 `raft.proto`
- `metadata_proto` 继续只承载 `metadata.proto`
- 当前没有让 `raft_core`、`raft_proto` 或 `metadata_proto` 强制链接 `storage_node_proto`
- 后续 T031/T032 可按需消费 `storage_node_proto`，但本任务不实现 service/client

## 是否修改 proto/storage_node.proto；如修改，说明原因

- 否

## 验证命令和结果

- `cmake --build --preset debug-ninja-low-parallel`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target storage_node_proto`
  - PASS
- `protoc --proto_path=proto --cpp_out=/tmp/storage_node_proto_check proto/storage_node.proto`
  - PASS
- `protoc --proto_path=proto --grpc_out=/tmp/storage_node_grpc_check --plugin=protoc-gen-grpc=$(command -v grpc_cpp_plugin) proto/storage_node.proto`
  - PASS
- `cmake --build --preset debug-ninja-low-parallel --target no_kv_surface_audit`
  - PASS
- `ctest --test-dir build/linux -R "write_chunk_contract|storage_upload|local_disk_chunk_store" --output-on-failure`
  - PASS

## Windows 验证判断

- 本任务复用了项目现有跨平台 protobuf / gRPC codegen 方式，没有新增单独的 Windows 特化路径或脚本
- 因此本次不新增 `T030-WIN`
- 当前仍未在真实 Windows 环境执行 build/test，报告里只说明未做实机验证，不伪造 PASS

## 是否通过 T030

- 是

## 是否可以进入 T031

- 是

## 当前任务发现的不合理点 / 警告 / 风险

- `check-prerequisites.sh` 仍错误指向 `specs/006-remove-kv-metadata-state-machine`
- `storage_node_proto` 已生成，但在 T031/T032 前仍没有真实 StorageNodeService / StorageNodeClient 使用这些 bindings

## 是否修正了高频文档，为什么

- 是，更新了 `tasks.md`
- 原因：将 T030 标记完成

## 是否更新 module-notes.md / AGENTS.md / contract 文档

- 未更新 `module-notes.md`
- 未更新 `AGENTS.md`
- 未更新 contract 文档

## common-risk-notes.md 读取结果

- 已读取
- T028、T027、T019、T014、T023、T025、T026 等现有风险仍存在

## common-risk-notes.md 新增/删除/保留情况

- 新增：无
- 删除：T029 schema 已落地但未接入 codegen 的边界风险
- 保留：T001、T009、T014、T016、T018、T019、T021、T023、T024、T025、T026、T027、T028
