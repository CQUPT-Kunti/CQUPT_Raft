# T071 StorageNode Identity Load/Create

## 1. 修改了哪些文件

- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t071-storage-node-identity-load-create.md`

## 2. storage_node_app 的 identity load/create 接入做了什么

- 现有 `apps/storage_node_app.cpp` 已经完成 T071 所需接入，本次没有再对源码做额外补丁。
- 启动阶段先通过 `ResolveStartupConfig(...)` 解析 `cluster_id`、`node_id`、`data_dir`、
  endpoint、capacity 和 identity source 边界。
- 随后通过 `EnsureNodeIdentity(...)` 调用
  `clusterdemo::LoadOrCreateNodeIdentity(...)`：
  - 优先加载当前 `data_dir` 下已有 `node.identity`
  - 若不存在，则按受控 first-start 边界创建新的 StorageNode identity
  - 创建路径使用 `NodeIdentityDurabilityMode::kRequired`
  - store 模式使用 `NodeIdentityStoreMode::kCreateNewOnly`
- 启动成功日志会输出：
  - `node_id`
  - `cluster_id`
  - `data_dir`
  - `identity_path`
  - `identity_source`
  - `identity_loaded_existing`
  - `identity_created_new`

## 3. 如何处理首次创建、重启复用、配置冲突和错误诊断

- 首次创建
  - `node.identity` 不存在时，创建新的 StorageNode identity，并要求 durable publish 成功。
- 重启复用
  - `node.identity` 已存在时，复用已有身份，不重新生成 `node_id`。
- 配置冲突
  - `ExpectedNodeIdentity` 明确约束 `cluster_id`、`node_id`、`node_type`、`source`。
  - 若 durable identity 与当前启动配置不一致，`LoadOrCreateNodeIdentity(...)` 返回冲突，
    app 直接失败退出，不静默覆盖。
- 错误诊断
  - identity 损坏、冲突、IO/权限失败、durability 不支持等错误都会通过
    `IdentityStartupError` 向上返回，并映射到明确的进程退出码。

## 4. 是否保持 StorageNode data-plane 与 metadata/control-plane 边界

- 是。
- identity load/create 只发生在 app 启动边界。
- 没有修改 chunk store、checksum、recovery、publish 语义。
- 没有引入 placement、upload/download、对象可见性或 Raft membership 逻辑。

## 5. 是否发现不合理点 / 警告 / 风险

- 当前 `storage_node_app` 的 identity 创建仍依赖启动前已经解析出的 `node_id`。
  对“完全没有本地 identity 且也没有 config/override node_id”的分配路径，仍要依赖
  T074/T072 或更上层启动编排配合，不属于本任务新增范围。
- 当前 `tasks.md` 相邻区域里 `T072/T073/T074/T075` 已存在工作区状态差异；
  本任务只新增 `T071=[X]`。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `risk-register.md`。
- `common-risk-notes.md` 在当前 feature 目录下不存在，因此未修改。

## 7. 验证命令和结果

### diff 检查

```bash
git diff -- apps/storage_node_app.cpp \
  modules/cluster/node_identity.h \
  modules/cluster/node_identity.cpp \
  modules/cluster/cluster_config.h \
  modules/cluster/cluster_config.cpp \
  specs/008-integrated-object-storage-system/contracts/app-cli.md \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t071-storage-node-identity-load-create.md
```

- 结果：已执行。
- 说明：本次没有新增 `apps/storage_node_app.cpp` 源码改动；该文件中的 T071 逻辑已存在，
  本任务完成了验证、任务勾选和任务报告落档。

### 最小构建验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-safe && cmake --build --preset debug-ninja-safe --target storage_node_app test_node_identity' \
  || echo "build lock busy, skip storage_node_app/test_node_identity build in this window"
```

- 结果：PASS。
- 成功编译 `storage_node_app`。

### identity 单元验证

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/tests/test_node_identity --gtest_brief=1' \
  || echo "build/test lock busy, skip test_node_identity binary in this window"
```

- 结果：PASS。
- 通过情况：`12 tests from 1 test suite ran`，`12 tests passed`。

### storage_node_app 最小 smoke test

```bash
flock -n /tmp/cqupt_raft_build.lock -c './build/linux/safe/storage_node_app --help' \
  || echo "build/test lock busy, skip storage_node_app --help in this window"
```

- 结果：PASS。
- 输出正确显示 `--config`、`--node_id`、`--data_dir`、`--listen` 启动参数。
