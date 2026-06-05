# T009 View Proto CMake 接入报告

## 1. 修改了哪些文件

- `CMakeLists.txt`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t009-view-proto-cmake.md`

`proto/view.proto` 未修改。本次检查后确认其语法、package 和当前 import 需求可直接接入现有生成流程。

## 2. view_proto 是如何接入 CMake 的

本次沿用项目现有的根 `CMakeLists.txt` protobuf / gRPC 生成方式，没有新增第二套 helper 或脚本。

具体接入方式：

1. 在 proto 输入列表中加入 `proto/view.proto`
   - 将 `view.proto` 加入 `PROTO_FILES`
   - 将 `view.proto` 加入 `GRPC_PROTO_FILES`

2. 在生成产物列表中加入 ViewNode 代码
   - `generated/view.pb.cc`
   - `generated/view.pb.h`
   - `generated/view.grpc.pb.cc`
   - `generated/view.grpc.pb.h`

3. 新增 `view_proto` target
   - 名称为 `view_proto`
   - include 目录与现有 proto target 保持一致，使用 `${GENERATED_DIR}`
   - link 风格与 `raft_proto` / `storage_node_proto` 一致，链接 `protobuf::libprotobuf` 与 `gRPC::grpc++`
   - Windows 下补充 `Ws2_32`，与现有 proto target 风格保持一致

说明：

- 本次没有把 `view_proto` 强行接入 `raft_core`、app target 或测试 target，因为当前 T009 只负责生成与 target 可用性，不提前引入后续业务依赖。

## 3. 是否保持 raft_proto、metadata_proto、storage_node_proto 不变

是。

- `raft_proto` 名称未改，原有源文件、include 和 link 语义未改。
- `metadata_proto` 名称未改，原有依赖 `common_proto` 的语义未改。
- `storage_node_proto` 名称未改，原有源文件、include 和 link 语义未改。
- 未修改已有 proto 字段编号、消息语义或调用方代码。

## 4. 是否发现不合理点 / 警告 / 风险

发现一个既有构建耦合点，但本任务未扩大其影响：

- 当前根 `CMakeLists.txt` 使用一组共享的 `add_custom_command()` 一次性生成所有 proto/gRPC 产物。
- 这意味着即使只构建 `view_proto`，底层生成步骤仍属于全量 proto 生成模型。
- 这是项目当前既有风格，不是本次引入的新问题；T009 仅按现有方式把 `view.proto` 纳入同一生成管线。

除此之外：

- `proto/view.proto` 当前无额外 import 依赖，不需要最小修正。
- 未发现会破坏现有 proto target 语义的接入冲突。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

- `common-risk-notes.md` 未修改。
- `risk-register.md` 未修改。

原因：

- 本任务范围限定为 proto 构建接入。
- 当前未发现需要在风险文档中单独登记的新协议或构建风险。

## 6. 验证命令和结果

执行命令：

```bash
git diff -- CMakeLists.txt proto/view.proto specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t009-view-proto-cmake.md
cmake --preset debug-ninja-low-parallel
cmake --build --preset debug-ninja-low-parallel --target view_proto
```

结果：

- `git diff`：用于确认本次修改面聚焦于 CMake 接入、任务状态和任务报告。
- `cmake --preset debug-ninja-low-parallel`：PASS，耗时约 2 秒。
- `cmake --build --preset debug-ninja-low-parallel --target view_proto`：PASS，耗时约 5 秒。

补充说明：

- 如果 `tasks.md` 中出现除 T009 外的其他已存在未提交改动，需要与本任务修改区分查看。
- 实际生成产物已落到：
  - `build/linux/generated/view.pb.h`
  - `build/linux/generated/view.pb.cc`
  - `build/linux/generated/view.grpc.pb.h`
  - `build/linux/generated/view.grpc.pb.cc`
- 配置阶段出现了现有 `tests/CMakeLists.txt` 中 `FetchContent_Declare` 相关的 CMake dev warning，但未影响 configure/build 成功，且与本次 `view_proto` 接入无直接关系。
