# T045 `view_node_app` thin startup

## 1. 修改了哪些文件

- `apps/view_node_app.cpp`
- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `modules/cluster/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t045-view-node-app-startup.md`

说明：
- 未修改 `proto/`
- 未修改 `CMakeLists.txt`
- 未修改测试文件
- 未修改 `specs/008-integrated-object-storage-system/contracts/app-cli.md`

## 2. view_node_app thin startup 做了什么

本任务在 `apps/view_node_app.cpp` 新增了一个薄启动入口，职责只覆盖启动装配、参数解析、identity 校验、错误展示和生命周期边界，不把 ViewNode registry 业务逻辑写进 app。

已实现的启动边界：

- 解析 CLI 参数：
  - `--config`
  - `--node_id`
  - `--data_dir`
  - `--listen`
  - `--help`
- 通过共享接口 `LoadClusterConfigFromJsonFile(...)` 读取统一 cluster config JSON，并先执行基础校验。
- 通过 `ResolveClusterNodeConfig(...)` 解析当前 ViewNode 的单节点配置。
- 当配置中存在且仅存在一个未命名 ViewNode 条目时，允许用显式 `--node_id` 绑定该条目，identity source 标记为 `kExplicitOverride`，避免静默 fallback。
- 通过 `LoadOrCreateNodeIdentity(...)` 加载或创建本地 `node.identity`，并校验：
  - `cluster_id`
  - `node_id`
  - `node_type=view`
  - `raft_id` 必须为空
  - `source`
- 初始化本地 `ViewNodeRegistry`，并用本节点的 `RegisterNode` 观测事实完成一次本地注册，作为 startup runtime boundary。
- 建立 gRPC 生命周期边界：
  - 绑定配置中的 listen endpoint
  - 启用默认 health check service
  - 建立并轮询 completion queue
  - 等待 `SIGINT` / `SIGTERM` 后优雅关闭
- 启动成功时输出可诊断信息：
  - `cluster_id`
  - `node_id`
  - `endpoint`
  - `data_dir`
  - `identity_source`

另外，为了让 role-specific app startup 共享统一配置读取逻辑，本任务最小补充了：

- `ClusterConfigLoadResult`
- `LoadClusterConfigFromJsonFile(const std::filesystem::path&)`

该接口只负责：

- 从统一 JSON 配置恢复 `ClusterConfig`
- 执行 `ValidateClusterConfig(...)`
- 返回显式诊断

它不承载 app 生命周期、registry 逻辑或 authority。

## 3. 如何处理 cluster config、node.identity、启动参数和错误诊断

### cluster config

- `--config` 必填；缺失时返回参数错误和非零退出码。
- 加载失败、JSON 解析失败、配置校验失败统一返回 config error。
- 共享 loader 内部补了最小 JSON 解析器，并将结果回灌为 `ClusterConfig` 再执行统一校验，避免在 app 内各自散落解析逻辑。

### node.identity

- 以 `cluster_id + node_id + node_type=view + raft_id=nullopt` 作为期望身份。
- 已有 identity 与期望不一致时返回明确错误，不静默覆盖。
- durability 不支持、写入失败、格式损坏、冲突等错误会映射到不同退出码。

### 启动参数

- `--node_id` 用于选择当前 ViewNode，或在单个未命名 ViewNode 条目场景下做受控本地 override。
- `--data_dir`、`--listen` 仅作为本地测试 override，不改变统一配置 authority。
- `--listen` 会做基本 `host:port` 格式校验。

### 错误诊断

- 参数错误：`kInvalidArgument`
- 配置错误：`kConfigError`
- identity 错误：`kIdentityError`
- durability / 平台不支持：`kUnsupported`
- 启动运行时错误：`kStartupError`
- 内部错误：`kInternalError`

## 4. 是否保持 ViewNode discovery-only / observation-only / non-authority 边界

保持。

本任务没有让 `view_node_app`：

- 保存 object manifest 权威副本
- 修改 Raft membership
- 参与 Raft quorum、leader election 或 commit
- 操作 StorageNode chunk payload
- 实现 ViewNode 自身共识

`view_node_app` 当前只做：

- discovery / observation runtime boundary 装配
- `node.identity` 管理
- 本地 registry 初始化
- gRPC 生命周期启动与退出

## 5. 是否发现不合理点 / 警告 / 风险

- 为满足统一配置驱动启动，本任务最小补了共享 `LoadClusterConfigFromJsonFile(...)`。当前实现使用手写 JSON 解析器，只覆盖项目当前生成器输出所需的 JSON 子集；如果后续要支持更宽松或外部来源配置，建议统一收敛到更完整的解析方案。
- 现有 `tasks.md` 在本任务开始前已经存在与 T045 无关的 `T046`、`T047` 勾选差异；本任务未回滚这些预置差异，只额外把 `T045` 从 `[ ]` 改为 `[X]`。
- 本任务没有接入具体 `ViewNodeServiceImpl` RPC adapter 到 app target。这里保持了 thin startup 边界，只建立 registry 和 gRPC 运行边界；更完整的 target / service wiring 仍应按任务边界在后续统一收敛。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

## 7. 验证命令和结果

### diff 检查

执行了：

```bash
git diff -- apps/view_node_app.cpp \
  specs/008-integrated-object-storage-system/contracts/app-cli.md \
  modules/cluster/cluster_config.h \
  modules/cluster/cluster_config.cpp \
  modules/cluster/node_identity.h \
  modules/cluster/node_identity.cpp \
  modules/view/view_service_impl.h \
  modules/view/view_service_impl.cpp \
  specs/008-integrated-object-storage-system/tasks.md
```

结果：

- 确认本任务实际改动集中在 `view_node_app` 和 `modules/cluster` 的最小共享加载接口。
- 未修改 proto、测试和 app target CMake。

### 构建

执行了：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target view_node_app'
```

结果：

- `view_node_app` 构建通过。

### 轻量 smoke

执行了：

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/view_node_app --help'
```

结果：

- 输出帮助信息，退出成功。

执行了：

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/view_node_app'
```

结果：

- 正确返回 `--config is required` 参数错误。

执行了：

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'timeout --preserve-status -s INT 2s ./build/linux/safe/view_node_app --config /tmp/tmp.mALCP1j9xF/cluster.json'
```

结果：

- 输出：

```text
view_node_app OK cluster_id=t045-view-smoke node_id=view-main endpoint=127.0.0.1:31145 data_dir=/tmp/tmp.mALCP1j9xF/view-main identity_source=config_generator
```

- 启动路径验证通过：配置加载、identity 创建、本地 registry 装配、端口绑定、信号退出均已跑通。

额外确认：

```bash
test -f /tmp/tmp.mALCP1j9xF/view-main/node.identity && sed -n '1,20p' /tmp/tmp.mALCP1j9xF/view-main/node.identity
```

结果：

- `node.identity` 已创建，内容中的 `cluster_id/node_id/node_type/source` 与启动配置一致。
