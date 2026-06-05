# T013 任务报告：durable node.identity load/store

## 1. 修改了哪些文件

- `modules/cluster/node_identity.cpp`
- `modules/cluster/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t013-node-identity-durable-store.md`

补充说明：

- 未修改 `modules/cluster/node_identity.h`。
- 未修改 proto、测试、app 入口或 CMake。

## 2. durable node.identity load/store 做了什么

本任务新增了 `modules/cluster/node_identity.cpp`，完整实现了 T012 头文件声明的以下接口：

- `ResolveNodeIdentityPath`
- `ValidateNodeIdentity`
- `ValidateNodeIdentityMatches`
- `LoadNodeIdentity`
- `StoreNodeIdentity`
- `LoadOrCreateNodeIdentity`
- `ToString(...)`
- `DescribeNodeIdentityIssue`

具体实现内容包括：

- `node.identity` 文件路径解析
  - 统一将身份文件定位到 `<data_dir>/node.identity`。

- identity 内容校验
  - 校验 `cluster_id`、`node_id`、`node_type`、`identity_version`、`raft_id` 的基本约束。
  - 明确 MetadataNode 必须带正数 `raft_id`，ViewNode/StorageNode 不得携带 `raft_id`。

- expected identity 匹配校验
  - 对已有 identity 与期望的 `cluster_id`、`node_id`、`node_type`、`raft_id`、`source` 做显式冲突检查。
  - mismatch 时返回 `validation + conflict`，不静默覆盖。

- `node.identity` 文件解析
  - 当前采用稳定文本 `key=value` 格式。
  - 解析字段：
    - `identity_version`
    - `cluster_id`
    - `node_id`
    - `node_type`
    - `raft_id`
    - `created_at_unix_ms`
    - `source`
  - 对缺字段、重复字段、未知字段、非法数值、非法枚举值返回明确 `corrupt` / `unsupported` 诊断。

- durable load
  - 支持从现有 `data_dir` 读取并校验 `node.identity`。
  - 对缺失、路径不是普通文件、内容损坏、格式不支持、与期望配置冲突等情况返回明确状态。

- durable store
  - 支持首次创建 `node.identity`。
  - 先写临时文件，再 flush，再 atomic publish，再按平台处理目录 durability。
  - `CreateNewOnly` 模式下如果已有 identity，直接冲突失败。
  - `ReplaceOnlyIfMatchesExpected` 模式下只允许“已有 identity 与 expected 匹配，且文件内容与待写 identity 完全一致”时重写；不允许把不同身份强行覆盖到已有文件上。

- load-or-create 语义
  - 先尝试 load。
  - 仅当文件不存在且 `require_existing=false` 时才尝试 create。
  - create 与其他并发创建发生竞争时，会再 load 一次，避免把“别人先成功写入同一个 identity”的正常情况误报成失败。

## 3. Linux/Windows durability contract 如何处理

### Linux

`required` durability 成功边界为：

1. 创建临时文件
2. 写入完整 identity 内容
3. 对临时文件执行 `fsync`
4. 原子 publish
   - `CreateNewOnly` 使用 `link + unlink staging`，避免覆盖已有最终文件
   - `ReplaceOnlyIfMatchesExpected` 使用 `rename`
5. 对 `data_dir` 执行目录 `fsync`

只有以上步骤全部完成，才返回：

- `status = kOk`
- `durable = true`

如果文件 `fsync`、原子 publish 或目录 `fsync` 任一步失败，都会返回明确 `durability_error` / `io_error`，不会静默成功。

### Windows

当前实现边界为：

1. 创建临时文件
2. 写入完整 identity 内容
3. `FlushFileBuffers`
4. `MoveFileExW(MOVEFILE_WRITE_THROUGH)` 发布最终文件

但由于当前实现没有独立目录 durability 的等价保证，所以：

- `NodeIdentityDurabilityMode::kRequired`
  - 明确返回 `kDurabilityError`
  - 明确说明“禁止把缺失的目录 durability 伪装成成功”

- `NodeIdentityDurabilityMode::kBestEffortForTests`
  - 允许返回 `kOk`
  - 但必须带 `durable=false`
  - 诊断中明确说明这是 best-effort publish，不宣称 durable success

这满足了“required durability operation 不允许 no-op success”的要求。

## 4. 是否发现不合理点 / 警告 / 风险

- 当前仓库存在与 T013 无关的全仓构建问题：
  - `cmake --build --preset debug-ninja-low-parallel` 在链接 `raft_demo` 时失败。
  - 失败摘要：`apps/main.cpp` 链接阶段找不到 `raftdemo::RaftNode::{RaftNode, Start, Stop, Describe, ~RaftNode}`。
  - 这不是本任务引入的错误；因为 `modules/cluster/node_identity.cpp` 已成功编译，`raft_core` 也已成功链接。

- Windows `required` durability 当前是显式失败而不是成功路径。
  - 这符合仓库的 durability contract，也比“伪装成功”安全。
  - 后续若要让 Windows `required` 模式通过，需要补足目录 durability 或明确升级 contract。

- 当前 `node.identity` 格式已落为稳定文本格式。
  - 后续如果要做格式迁移，需要补充版本升级和兼容测试，不能直接改写字段语义。

## 5. 是否修改 `common-risk-notes.md` 或 `risk-register.md`

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。

本任务只实现 node identity 的本地 durable load/store 边界，没有新增需要同步到全局风险清单的跨模块策略变更。

## 6. 验证命令和结果

执行：

```bash
git diff -- modules/cluster/node_identity.cpp modules/cluster/node_identity.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t013-node-identity-durable-store.md
```

结果：PASS。

补充说明：

- `git diff -- <path>` 不会直接显示未跟踪的新文件内容，但可用于确认 `tasks.md` 和 `module-notes.md` 的修改范围。
- 本任务新增文件为 `modules/cluster/node_identity.cpp` 和本报告文件。
- `modules/cluster/module-notes.md` 与 `tasks.md` 在本任务开始前已存在其他未提交改动；本任务只追加了 T013 所需内容，并将 T013 标记为 `[X]`。

执行：

```bash
cmake --preset debug-ninja-low-parallel
```

结果：PASS。

执行：

```bash
cmake --build --preset debug-ninja-low-parallel --target raft_core
```

结果：PASS。

说明：

- `modules/cluster/node_identity.cpp` 已成功编译。
- `libraft_core.a` 已成功链接，说明本任务新增实现没有破坏 `raft_core`。

执行：

```bash
cmake --build --preset debug-ninja-low-parallel
```

结果：FAIL。

失败摘要：

- 目标：`raft_demo`
- 分类：现存全仓链接失败，非 T013 直接引入
- 关键错误：
  - `undefined reference to raftdemo::RaftNode::Start()`
  - `undefined reference to raftdemo::RaftNode::Describe() const`
  - `undefined reference to raftdemo::RaftNode::Stop()`
  - `undefined reference to raftdemo::RaftNode::RaftNode(...)`
  - `undefined reference to raftdemo::RaftNode::~RaftNode()`

结论：

- 当前可以确认 T013 对应的 `node_identity.cpp` 和 `raft_core` 目标通过。
- 后续 T014 仍需要补专门的 `node_identity` 单元测试；本任务没有新增测试文件。
