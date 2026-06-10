# T015 ViewNode Registry 接口任务报告

## 1. 修改文件

- `modules/view/view_registry.h`
- `modules/view/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t015-view-registry-interface.md`

## 2. `view_registry.h` 定义内容

- 新增 `viewdemo` 命名空间和 `ViewNodeRegistry` 纯接口类。
- 定义节点与观测类型：`ViewNodeType`、`ViewNodeLivenessState`、`ViewNodeHealth`、`ViewNodeDiskPressure`、`MetadataMembershipObservedState`、`MetadataRaftObservedRole`。
- 定义可诊断结果类型：`ViewRegistryStatusCode`、`ViewRegistryIssueCode`、`ViewRegistryDiagnostic`、`ViewRegistryResponseSummary`。
- 定义注册和观测事实：`NodeRegistration`、`MetadataLeaderHint`、`MetadataNodeObservation`、capacity/health/load/failure domain report。
- 定义 registry 快照和发现结果：`ViewNodeSnapshot`、`DiscoverMetadataResult`、`DiscoverStorageResult`、`ClusterViewSnapshot`、`GetClusterViewResult`。
- 定义接口边界：`RegisterNode`、`HeartbeatNode`、`LookupNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView`、`size`、`config`。
- 查询接口显式接收 `now_unix_ms`，便于 T016 用确定性时间源实现 liveness transition 和单元测试。

## 3. discovery-only / observation-only / non-authority 边界

- 已在头文件注释中说明 registry 只表达 discovery / observation facts。
- 未引入 object manifest、chunk payload、`CommitObject`、Raft membership mutation 或 quorum 变更相关接口。
- MetadataNode 的 leader hint、observed role 和 membership state 都以观测事实表达，不赋予 ViewNode voter 提升或 leader election 权限。

## 4. 是否只定义接口

- 是。本任务只新增头文件类型、接口声明和轻量 `ok()` / `IsSuccessfulStatus()` inline。
- 未实现注册、幂等判断、heartbeat sequence 排序、liveness transition、snapshot 过滤或 discovery 排序。
- 未新增 `view_registry.cpp`，该实现属于 T016。
- 未加入 proto/gRPC include；T018/T019 的 service adapter 后续负责字段映射。

## 5. 不合理点 / 警告 / 风险

- `CMakeLists.txt` 已有 008 planned source placeholder 指向 `modules/view/view_registry.cpp`，但当前构建逻辑只收集已存在源文件；T015 不需要创建 `.cpp`。
- `proto/view.proto` 已完成 T008，`view_registry.h` 的类型边界与 proto 概念对齐，但没有直接依赖生成代码，避免把 registry 头文件绑定到 gRPC/protobuf。
- 后续 T016 需要确保高并发读写路径采用可测试的锁策略或快照策略，并保持 stale heartbeat 不覆盖新事实。
- 最终 diff 中观察到 `tasks.md` 的 T010、T012 也已标记为 `[X]`；这些不是本任务修改内容。本任务只新增 T015 勾选，未验证 T010/T012。
- 最终状态中观察到未跟踪的 `tests/integrated_object_storage_e2e_test.cpp`；该文件不是本任务产生或修改的测试文件，本任务未修改测试。

## 6. common-risk-notes.md / risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 本任务风险已记录在本报告中，未发现需要扩大到风险登记文件的事项。

## 7. 验证命令和结果

```bash
git diff -- modules/view/view_registry.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t015-view-registry-interface.md
```

结果：PASS，目标路径 diff 显示 `modules/view/module-notes.md` 同步更新，以及 `tasks.md` 中 T015 已标记为 `[X]`。

补充说明：`modules/view/view_registry.h` 和本报告是新增未跟踪文件，普通 `git diff -- <path>` 不展示其正文；已通过 `git status --short -- modules/view/view_registry.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t015-view-registry-interface.md` 确认目标文件状态。

```bash
printf '#include "view/view_registry.h"\n' | g++ -std=c++20 -Imodules -x c++ -fsyntax-only -
```

结果：PASS，头文件语法、include guard、命名空间和声明可被 C++20 编译器解析。

编译说明：本任务只定义头文件接口，不新增业务实现、不修改 proto/CMake/测试；未运行完整 CMake / build。
