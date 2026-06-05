# T020 任务报告：ViewNode client adapter 接口

## 1. 修改了哪些文件

- `modules/view/view_client.h`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t020-view-client-interface.md`

## 2. `view_client.h` 定义了哪些 client adapter 接口边界

本次在 `modules/view/view_client.h` 新增了 `ViewNodeClient` 头文件接口，只定义边界，不实现 RPC 调用逻辑。

新增内容包括：

- `ViewNodeClientConfig`
  - 为注册、心跳、discovery、cluster view 查询分别预留默认超时边界。
  - 预留 `wait_for_ready` 默认行为，便于 T021 统一映射到 gRPC `ClientContext`。

- `ViewNodeClientCallOptions`
  - 支持单次调用覆盖默认 timeout / `wait_for_ready`。

- `ViewNodeClientCallDiagnostics`
  - 明确 transport 诊断边界，包含 `request_id`、`cluster_id`、`node_id`、`target_endpoint`、`grpc_status_code`、错误消息、错误详情、effective timeout 和 retryable 标记。
  - 让后续 app、transfer 或状态命令可以保留工业化排障信息，而不是只拿一个布尔结果。

- `ViewNodeClientCallResult<Result>`
  - 统一包装 transport 结果和业务结果，区分 “RPC 传输成功” 与 “ViewNode 返回的 observation/result 成功”。
  - `ok()` 只有在 transport 成功且底层 `Result::ok()` 成功时才返回 true。

- `ViewNodeClient` 方法边界
  - `RegisterNode`
  - `HeartbeatNode`
  - `DiscoverMetadata`
  - `DiscoverStorage`
  - `GetClusterView`

这些方法统一复用 `modules/view/view_registry.h` 中已经定义好的 `RegisterNodeRequest`、`HeartbeatNodeRequest`、`DiscoverMetadataRequest`、`DiscoverStorageRequest`、`GetClusterViewRequest` 以及对应结果类型，说明 client adapter 的职责是：

- 把模块内稳定的 discovery / observation 语义类型映射到 `ViewNodeService` RPC。
- 把 RPC transport 诊断补充回模块语义结果。
- 暴露 leader hint、endpoint snapshot、warnings/diagnostics、liveness、membership observed state 等观测结果。
- 不把 proto/gRPC 细节继续散落到上层业务模块。

## 3. 是否保持 ViewNode non-authority 边界

是，已保持。

头文件中的类注释和方法注释明确写明：

- `ViewNodeClient` 不负责对象 `COMMITTED` 可见性。
- 不负责 Raft membership 变更。
- 不负责 quorum 计算或降低 quorum。
- `DiscoverMetadata` 返回的 `leader hint` 只是观测提示，调用方仍必须处理 `MetadataService NOT_LEADER`。
- `DiscoverStorage` 返回的 endpoint snapshot 不是 object manifest，也不是对象可见性的依据。
- 不操作 StorageNode payload。
- 不承担 transfer 编排或 app 启动循环。

因此本次接口定义仍然严格停留在 discovery-only / observation-only client adapter 边界。

## 4. 是否保持只定义接口、不实现 RPC 调用逻辑

是，已保持。

本任务仅新增头文件声明：

- 只声明构造函数、配置结构、调用选项、transport 诊断结构和 5 个 RPC 适配方法签名。
- 没有新增 `.cpp`。
- 没有实现 `ClientContext`、stub 调用、proto 映射、重试、超时处理或错误分类逻辑。

这些实现工作留给 T021。

## 5. 是否发现不合理点 / 警告 / 风险

- 发现 `modules/view/view_registry.h` 当前已经存在未提交内容，并使用 `viewdemo` 命名空间与模块语义类型。本次 `view_client.h` 已主动对齐该命名空间和类型风格，以避免 T020/T015/T016/T021 之间产生接口漂移。
- 当前 `tasks.md` 里 T009 仍显示未完成，仓库中也未检索到 `view_proto` / `view.grpc.pb.h` 的 CMake 接入痕迹。因此本任务没有承诺当前分支一定能直接通过完整编译；T021 或后续集成前仍需要确认 T009 的真实落地状态。
- `ViewNodeClientCallResult<Result>` 选择复用 `view_registry.h` 结果类型并叠加 transport 诊断，这有助于边界清晰，但也意味着 T021 需要实现 proto <-> registry 类型映射函数；这是预期成本，不是阻塞问题。

## 6. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。

本任务只定义 client adapter 接口边界，没有新增需要写入风险总表的实现级变更。

## 7. 验证命令和结果

执行：

```bash
git diff -- modules/view/view_client.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t020-view-client-interface.md
```

结果：PASS，用于确认 `tasks.md` 的改动范围以及观察目标路径上的已有工作区差异。

补充说明：

- `git diff -- <path>` 不展示未跟踪的新文件，因此不会直接显示新建的 `modules/view/view_client.h` 和本任务报告文件内容。
- 该 diff 同时暴露了本任务开始前已存在的外部改动：`modules/view/module-notes.md` 已有未提交修改，`tasks.md` 中还包含 T010、T012、T015、T022 的既有勾选变化。
- 本任务只新增了 T020 的 `[X]` 勾选，没有修改 `module-notes.md`，也没有触碰其他任务对应文件。

补充验证：

```bash
git status --short -- modules/view/view_client.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t020-view-client-interface.md
```

结果：PASS，确认：

- `modules/view/view_client.h` 为本任务新增文件。
- `specs/008-integrated-object-storage-system/task-reports/t020-view-client-interface.md` 为本任务新增文件。
- `specs/008-integrated-object-storage-system/tasks.md` 为已修改文件。

头文件边界审查：

- `modules/view/view_client.h` 已使用 `#pragma once`。
- 命名空间与 `modules/view/view_registry.h` 对齐为 `viewdemo`。
- 接口只包含声明、注释和轻量 `ok()` / `transport_ok()` inline，不包含 gRPC 调用实现。
- 头文件通过人工语法审查，未发现明显的声明级冲突。

编译说明：

- 未运行 `cmake --preset debug-ninja-low-parallel`。
- 原因：当前 `tasks.md` 中 T009 仍未勾选，且仓库内未检索到 `view_proto` / `view.grpc.pb.h` 的 CMake 接入痕迹；本任务又只新增接口头文件、不新增实现，因此本次以头文件语法与边界审查为主，没有扩大到构建系统验证。
