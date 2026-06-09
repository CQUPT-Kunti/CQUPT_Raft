# T062 任务报告

## 1. 修改了哪些文件

- `CMakeLists.txt`
- `apps/storage_node_app.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`

## 2. storage_node_app 的 ViewNode registration / heartbeat loop 做了什么

- 在 `storage_node_app` 启动配置中补充了：
  - `view_endpoints`
  - `ViewNodeClientConfig`
  - `heartbeat_interval`
- 对 `storage_node_app` target 做了最小构建修正：
  - 追加 `view_proto` 链接依赖
  - 原因是本任务接入 `ViewNodeClient` 后，target 需要显式拿到 `view.proto` 生成符号
- 从统一 cluster config 读取全部 `ViewNode` endpoint，并基于 `registration_timeout` / `discovery_rpc_timeout` 构造 `ViewNodeClient` 的 register/heartbeat 超时配置。
- 在 app 内新增最小 helper，把本地 `StorageNodeRegistryFacts` 映射成 `viewdemo::NodeRegistration`：
  - 上报 `cluster_id`、`node_id`、`endpoint`
  - 上报 `data_dir_fingerprint`
  - 上报 `health`、`capacity`、`load`
  - 上报 `failure_domain`
- 在 gRPC server 成功绑定后，先尝试向已配置的 `ViewNode` 列表顺序注册；任一 endpoint 注册成功后再进入正常运行。
- 新增受控后台 heartbeat 线程：
  - 按 `heartbeat_interval` 周期上报当前本地 registry facts
  - heartbeat 失败时打印明确诊断
  - 失败后重新尝试向已配置的 `ViewNode` 列表注册，支持 seed endpoint 切换
  - 线程受 `g_stop_requested` 控制，shutdown 时正常退出，不无界阻塞

## 3. 如何保持 StorageNode data-plane、ViewNode observation 和 metadata authority 的边界

- 这次改动只在 `apps/storage_node_app.cpp` 装配 `ViewNodeClient`、注册请求和 heartbeat 循环。
- `ViewNode` 只接收 StorageNode 的观测事实，不接收 object manifest、chunk payload 或 COMMITTED 可见性决策。
- 未修改 `chunk store`、checksum、durable publish、recovery 语义。
- 未修改 `MetadataNode`、Raft quorum、leader election、commit 或 membership 规则。
- heartbeat 只刷新 discovery / observation facts，不实现 placement policy，也不把 ViewNode 提升为 metadata authority。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前 `storage_node_app` 的 heartbeat 上报事实主要来自本地 `StorageNodeRegistry` 快照；如果后续需要更精确的实时容量/负载采样，还需要在后续任务中补更细粒度的数据源接线。
- 当前多 ViewNode 策略是“顺序尝试已配置 endpoint，失败时重注册切换”，属于 app 启动侧最小容错，不代表 ViewNode 已具备自身强一致或复制能力。
- 本任务未实现 `storage_client status`、placement adapter 或 metadata startup registration；这些边界仍由 T061/T063/T064/T065 继续完成。
- 当前工作区还存在与本任务无关的 `T061` / `metadata_node_app` 相关未提交差异；本任务未回滚这些既有修改。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 6. 验证命令和结果

- diff 检查：

```bash
git diff -- apps/storage_node_app.cpp CMakeLists.txt modules/view/view_client.h modules/view/view_client.cpp modules/store/node/module-notes.md specs/008-integrated-object-storage-system/contracts/app-cli.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t062-storage-node-registration-heartbeat.md
```

- 构建命令：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_node_app' \
|| echo "build lock busy, skip storage_node_app build in this window"
```

- smoke 命令：

```bash
./build/linux/safe/storage_node_app --help
```

- 结果：
  - `git diff` 已执行，确认本任务核心改动位于 `apps/storage_node_app.cpp`，并包含一个最小 `CMakeLists.txt` 链接修正。
  - `flock` 构建成功，`storage_node_app` 已完成单目标编译。
  - `./build/linux/safe/storage_node_app --help` 已执行，返回用法文本，smoke PASS。
