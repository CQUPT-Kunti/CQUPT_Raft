# T072 ViewNode identity load/create 接入报告

## 1. 修改了哪些文件

- `apps/view_node_app.cpp`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t072-view-node-identity-load-create.md`

未修改：

- `modules/cluster/node_identity.h`
- `modules/cluster/node_identity.cpp`
- `modules/cluster/cluster_config.h`
- `modules/cluster/cluster_config.cpp`
- `specs/008-integrated-object-storage-system/contracts/app-cli.md`
- `common-risk-notes.md`
- `risk-register.md`

## 2. view_node_app 的 identity load/create 接入做了什么

- 保留 `view_node_app` 的 thin startup 边界，只在 app 启动阶段接入 durable `node.identity` 的 `LoadOrCreateNodeIdentity(...)` 调用。
- 将 identity 启动结果收敛为 `IdentityStartupState`，显式保留：
  - `identity`
  - `identity_path`
  - `loaded_existing`
  - `created_new`
  - `durable`
- 启动成功时输出：
  - `cluster_id`
  - `node_id`
  - `data_dir`
  - `identity_path`
  - `identity_source`
  - `identity_state(created|loaded)`
  - `identity_durable(true|false)`

## 3. 如何处理首次创建、重启复用、配置冲突和错误诊断

- 首次创建：
  - 若 `data_dir` 下不存在 `node.identity`，按当前 ViewNode 启动配置创建并持久化新的 ViewNode identity。
- 重启复用：
  - 若 `node.identity` 已存在，则优先复用现有 durable identity，不在每次启动时重新生成 `node_id`。
- 配置冲突：
  - 通过 `ExpectedNodeIdentity` 约束 `cluster_id`、`node_id`、`node_type=view`、`source` 和 `raft_id=nullopt`。
  - 若本地 identity 与当前配置不匹配，则直接失败，不静默覆盖。
- 错误诊断：
  - 将 `data_dir` 带入 identity 启动失败信息，便于定位是哪一个目录下的 `node.identity` 出错。
  - 退出码继续按现有 `MapIdentityExitCode(...)` 映射，不改变既有 app 语义。

## 4. 是否保持 ViewNode discovery-only / observation-only / non-authority 边界

- 是。
- 本次改动只处理 app 启动期的 identity load/create 与错误展示。
- 未引入任何 ViewNode 共识、高可用、Raft membership、quorum、commit 或 object manifest authority 逻辑。
- 未让 ViewNode 操作 StorageNode payload 或扩展 discovery/registry 业务语义。

## 5. 是否发现不合理点 / 警告 / 风险

- `view_node_app.cpp` 在本任务开始前已经有一版 `LoadOrCreateNodeIdentity(...)` 接入；本次是在该基础上补齐 first-start/reuse 的可诊断输出，而不是从零引入 identity 逻辑。
- 当前 `view_node_app` 成功日志仍使用单行文本输出；对脚本友好，但如果后续需要更稳定的机器可读诊断，建议在后续任务中统一结构化输出格式。
- 当前仓库实际可构建 target 名是 `view_node_app`；identity 单测 target 名实际为 `test_node_identity`，不是任务文字中的 `node_identity_test`。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`
- 未修改 `risk-register.md`

## 7. 验证命令和结果

执行命令：

```bash
git diff -- apps/view_node_app.cpp modules/cluster/node_identity.h modules/cluster/node_identity.cpp modules/cluster/cluster_config.h modules/cluster/cluster_config.cpp specs/008-integrated-object-storage-system/contracts/app-cli.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t072-view-node-identity-load-create.md
git diff --check -- apps/view_node_app.cpp specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t072-view-node-identity-load-create.md
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target view_node_app'
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/view_node_app --help'
./build/linux/safe/view_node_app --config /nonexistent/path.json
```

结果：

- `git diff`：PASS。改动限定在 `view_node_app.cpp`、`tasks.md` 和任务报告。
- `git diff --check`：PASS。未发现空白错误。
- `cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target view_node_app`：PASS。
- `./build/linux/safe/view_node_app --help`：PASS。正确输出 usage 和参数说明。
- `./build/linux/safe/view_node_app --config /nonexistent/path.json`：PASS。按预期返回 config error：`failed to open cluster config file: /nonexistent/path.json`，说明 identity 接入未破坏参数/配置错误边界。
