# T019 View Service Impl 实现报告

## 1. 修改了哪些文件

- `modules/view/view_service_impl.cpp`
- `modules/view/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t019-view-service-impl.md`

未修改 `proto/`、测试文件和 app 入口。

## 2. ViewNode service adapter 做了什么

本次补充了 `modules/view/view_service_impl.cpp`，实现了 `viewdemo::ViewNodeServiceImpl` 的同步 unary gRPC adapter，主要工作如下：

- 实现 `RegisterNode`、`HeartbeatNode`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView`
- 把 `proto/view.proto` 的 request 映射为 `ViewNodeRegistry` 的请求类型
- 把 `ViewNodeRegistry` 的 summary、snapshot、leader hint、warnings 映射回 proto response
- 为 discovery / cluster view 查询接入 `now_unix_ms` 时间源注入，默认回退到系统时间
- 对 registry 未配置、空 request/response、内部异常等适配层故障返回明确 gRPC 错误

实现保持 adapter 只做边界转换，不挪走 registry 的核心状态职责。

## 3. 是否保持 ViewNode discovery-only / observation-only / non-authority 边界

是。

- 注册和心跳只传递 discovery / observation facts
- `DiscoverMetadata` 返回的 leader hint 仍然只是观测提示，不是强一致 authority
- `DiscoverStorage` 返回的是 StorageNode 观测事实，不代表对象可见性
- service adapter 不修改 Raft membership、quorum、commit 规则或 election 规则
- service adapter 不保存 object manifest 权威副本，不接收或返回 chunk payload

## 4. 是否发现不合理点 / 警告 / 风险

发现一个需要后续调用方注意的点：

- `proto/view.proto` 里的多个筛选字段使用 proto3 普通 `bool`
- 这类字段在未显式设置时会落成 `false`，无法区分“调用方未设置”和“调用方明确要求 false”
- 因此后续 T020/T021 的 client adapter 应显式设置 `prefer_leader`、`live_only`、`include_dead_nodes`、`include_warnings` 等字段，避免依赖 registry 结构体里的默认值

此外，本次实现选择：

- 对 registry 业务状态一律使用 `grpc::Status::OK` + response summary/warnings 返回
- 仅把 adapter 自身的空指针、未配置 registry、内部异常当作 gRPC transport/internal 故障

该选择与当前项目“transport 与业务状态分层”的风格更一致。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

- `common-risk-notes.md` 未修改
- `risk-register.md` 未修改

原因：

- 本任务只实现 ViewNode service adapter，不改变协议语义、Raft 安全边界或持久化格式

## 6. 验证命令和结果

执行命令：

```bash
git diff -- modules/view/view_service_impl.cpp modules/view/view_service_impl.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t019-view-service-impl.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe' \
  || echo "build lock busy, skip build in this window"
```

结果：

- `git diff`：用于确认修改面聚焦在 T019 所需的 adapter 实现、必要说明同步、任务状态和任务报告
- 构建结果：PASS
- 使用了构建锁，并成功获得 `/tmp/cqupt_raft_build.lock`
- 执行命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe'
```

- 总耗时：约 274 秒
- 构建日志：`tmp/test-logs/t019-build-safe-with-lock.log`

补充说明：

- 如果构建锁被占用，本窗口不会等待，也不会重复发起构建；会在结果中明确记录为“未执行 build，待统一验证”
- `specs/008-integrated-object-storage-system/tasks.md` 中除本次将 `T019` 从 `[ ]` 改为 `[X]` 外，还存在进入本任务前就已存在的未提交状态变更：`T021`、`T025` 已为 `[X]`；本次未回退这些既有改动
