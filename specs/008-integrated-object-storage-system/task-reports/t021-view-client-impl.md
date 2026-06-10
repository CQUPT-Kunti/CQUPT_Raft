# T021 - ViewNode client adapter 实现

## 1. 修改了哪些文件

- `modules/view/view_client.cpp`
  - 新增 ViewNode client adapter 实现，完成 RegisterNode / HeartbeatNode / DiscoverMetadata / DiscoverStorage / GetClusterView 的 gRPC 调用适配。
- `modules/view/module-notes.md`
  - 补充 `view_client.h` / `view_client.cpp` 的职责边界、timeout/diagnostic 语义和 non-authority 约束说明。
- `specs/008-integrated-object-storage-system/tasks.md`
  - 仅将 `T021` 从 `[ ]` 更新为 `[X]`。

本任务未修改：

- `modules/view/view_client.h`
- `common-risk-notes.md`
- `risk-register.md`
- `proto/`
- `tests/`
- `apps/`

## 2. ViewNode client adapter 做了什么

`modules/view/view_client.cpp` 当前实现了以下内容：

1. 构造与配置
- 支持从 `ViewNodeService::StubInterface` 或 `grpc::Channel` 构造 `ViewNodeClient`
- 对空 stub 显式报错，避免静默空调用

2. RPC 调用适配
- 实现 `RegisterNode`
- 实现 `HeartbeatNode`
- 实现 `DiscoverMetadata`
- 实现 `DiscoverStorage`
- 实现 `GetClusterView`

3. 请求构造
- 把本地 `viewdemo` 请求类型映射到 `proto/view.proto` 消息
- 覆盖节点注册、heartbeat observation、metadata observation、leader hint、cluster view 过滤参数等字段

4. 返回值转换
- 把 proto `summary`、`snapshot`、`leader_hint`、warning 转回本地 `viewdemo` 结果类型
- 将 response warning 转换为本地 `ViewRegistryDiagnostic`
- 保留 MetadataNode / StorageNode discovery 结果的“观测事实”语义

5. transport 诊断
- 为每次 RPC 记录 `grpc_status_code`、`grpc_error_message`、`grpc_error_details`
- 记录 `effective_timeout` 和 `wait_for_ready`
- 对 transport failure 生成本地 summary 和 diagnostic，避免吞掉关键错误
- 对 `DEADLINE_EXCEEDED` / `UNAVAILABLE` / `RESOURCE_EXHAUSTED` 这类 transport failure 给出 retryable 标记

6. 调用选项
- 支持按 RPC 类型使用默认 timeout
- 支持单次调用覆盖 timeout / `wait_for_ready`
- timeout 通过 `grpc::ClientContext` deadline 传递，不在 client adapter 内做业务重试或编排

## 3. 是否保持 ViewNode non-authority 边界

已保持。

当前实现只做 transport + proto 映射，没有引入以下越权行为：

- 不决定对象是否 `COMMITTED` 可见
- 不修改 Raft membership
- 不降低 Raft quorum
- 不把 leader hint 当作强一致事实
- 不把 StorageNode discovery 结果当作 object manifest
- 不操作 StorageNode chunk payload
- 不实现 upload/download 编排
- 不实现 app 注册/心跳循环

## 4. 是否发现不合理点 / 警告 / 风险

有两点需要记录：

- 当前 `tasks.md` 工作区里除了 `T021` 之外，还存在其他外部勾选差异；本次 diff 中可见 `T019`、`T025` 已和基线不同。本任务没有修改或判断这些外部任务是否真实完成。
- 本次未新增专门的 `ViewNodeClient` 单元测试；当前先完成 adapter 实现，后续若需要更强回归保护，建议补 fake stub / in-process service 的 client 测试。

补充说明：

- `modules/view/view_service_impl.cpp` 当前已存在于工作区，因此本次没有为构建缺口去扩展 T019 范围。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`，未修改 `risk-register.md`。

## 6. 验证命令和结果

已执行：

1. 差异检查

```bash
git diff -- modules/view/view_client.cpp modules/view/view_client.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md
git status --short -- modules/view/view_client.cpp modules/view/view_client.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t021-view-client-impl.md
```

结果：

- `modules/view/view_client.cpp` 已新增
- `modules/view/module-notes.md` 已补充 client adapter 边界说明
- `modules/view/view_client.h` 未修改
- `tasks.md` 已将 `T021` 勾选为完成
- `git diff` 同时显示了工作区内已有的其他任务勾选差异，这些不是本任务新增

2. 按要求尝试带锁构建

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe' || echo "build lock busy, skip build in this window"
```

结果：

- `build lock busy, skip build in this window`
- 按任务要求，本窗口未继续重复启动 build，待统一验证

## 结论

- T021 的实现与任务报告已完成落地。
- 本窗口未执行最终 build 验证，原因是构建锁被占用。
- 待统一窗口完成一次受锁保护的 configure/build 后，可继续推进后续任务。
