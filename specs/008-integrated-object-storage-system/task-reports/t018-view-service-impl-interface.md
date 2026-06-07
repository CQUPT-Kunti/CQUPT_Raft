# T018 View Service Impl 接口报告

## 1. 修改了哪些文件

- `modules/view/view_service_impl.h`
- `modules/view/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t018-view-service-impl-interface.md`

未修改 `proto/`、测试文件和 app 入口。

## 2. view_service_impl.h 定义了什么 service adapter 边界

本次新增 `modules/view/view_service_impl.h`，定义了 `viewdemo::ViewNodeServiceImpl`，它是：

- `view::ViewNodeService::Service` 的同步 gRPC service adapter 声明
- `proto/view.proto` 与 `viewdemo::ViewNodeRegistry` 之间的适配边界

头文件中定义了：

- `ViewNodeServiceImplConfig`
  - 预留 `now_unix_ms` 时间源注入点，供 T019 和测试使用
- `ViewNodeServiceImpl`
  - 构造函数接收 `std::shared_ptr<ViewNodeRegistry>`，支持依赖注入
  - 预留以下 RPC override 声明：
    - `RegisterNode`
    - `HeartbeatNode`
    - `DiscoverMetadata`
    - `DiscoverStorage`
    - `GetClusterView`
  - 暴露 `registry()` 与 `config()` 访问边界
  - 禁止拷贝/移动，避免 service 生命周期与 registry 依赖关系被隐式复制

## 3. 是否保持只定义接口、不实现 RPC 逻辑

是。

- 本次只新增头文件声明和中文注释
- 没有新增 `.cpp` 实现
- 没有实现任何 proto 到 registry 的字段映射
- 没有实现任何 gRPC `Status` 返回逻辑
- 没有实现 app 启动或 server 装配逻辑

这些内容留给 T019。

## 4. 是否保持 ViewNode non-authority 边界

是。

头文件注释中明确了以下边界：

- 不作为 Raft membership authority
- 不修改 Raft membership
- 不降低 quorum
- 不决定对象 `COMMITTED` 可见性
- 不保存 object manifest 权威副本
- 不操作 StorageNode payload
- 不承载 `view_node_app` 启动逻辑

同时也同步更新了 `modules/view/module-notes.md`，把 `view_service_impl.h` 的职责和禁止事项写清楚。

## 5. 是否发现不合理点 / 警告 / 风险

发现一个合理但需要后续实现阶段注意的点：

- `ViewNodeRegistry` 的查询接口显式要求 `now_unix_ms`
- 因此 service adapter 不能在实现里偷偷依赖不可控的全局时间源
- 本次已在接口层预留 `now_unix_ms` 注入配置，避免 T019 把时间依赖写死

此外：

- 当前选择同步 unary `view::ViewNodeService::Service` 作为 adapter 基类，符合现有项目 gRPC service adapter 风格
- 如果未来需要 async/callback server，应作为后续单独实现决策，而不是在 T018 的头文件里提前扩展复杂度

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改。

- `common-risk-notes.md` 未修改
- `risk-register.md` 未修改

原因：

- 本任务只定义接口边界，没有改变行为或协议语义

## 7. 验证命令和结果

执行命令：

```bash
git diff -- modules/view/view_service_impl.h modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t018-view-service-impl-interface.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe' \
  || echo "build lock busy, skip build in this window"
```

结果：

- `git diff`：用于确认本次修改面聚焦在 service adapter 头文件、必要说明同步、任务状态和任务报告
- 构建结果：PASS
- 使用了构建锁，并成功获得 `/tmp/cqupt_raft_build.lock`
- 执行命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe'
```

- 总耗时：约 702 秒
- 构建日志：`tmp/test-logs/t018-build-safe-with-lock.log`

补充说明：

- 如果构建锁被占用，本窗口不会等待，也不会重复发起构建；会在结果中明确记录为“未执行 build，待统一验证”
- `specs/008-integrated-object-storage-system/tasks.md` 中除本次将 `T018` 从 `[ ]` 改为 `[X]` 外，还存在进入本任务前就已存在的未提交状态变更：`T014`、`T017`、`T024` 已为 `[X]`；本次未回退这些既有改动
