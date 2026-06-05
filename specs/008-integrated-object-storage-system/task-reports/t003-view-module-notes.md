# T003 ViewNode 模块说明任务报告

## 1. 修改文件

- `modules/view/AGENTS.md`
- `modules/view/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t003-view-module-notes.md`

## 2. AGENTS.md 补充约束

- 明确 `modules/view/` 只提供 discovery-only / observation-only 能力。
- 明确 ViewNode 可维护节点注册、heartbeat、liveness、服务发现、leader hint 和冲突诊断。
- 明确禁止把 ViewNode 写成 Raft membership authority、object manifest authority 或 CommitObject / chunk 数据执行方。
- 要求注册、心跳、发现、liveness 和冲突处理可测试、可诊断。
- 要求新增关键结构、字段、状态枚举和超时策略时同步更新 `module-notes.md`。
- 明确本目录不写任务执行流水账。

## 3. module-notes.md 补充边界

- 说明 ViewNode 的模块职责：节点注册、心跳、服务发现、状态观测和 leader hint 展示。
- 补充核心概念：`NodeRegistration`、`Heartbeat`、`Liveness`、`DiscoverMetadata`、`DiscoverStorage`、`GetClusterView`、`leader hint`。
- 写明 Non-Authority Boundary：ViewNode 不保存 object manifest 权威副本，不参与 `CommitObject`，不操作 chunk 数据，不修改 Raft membership，不降低 quorum，不参与 leader election，不决定对象 `COMMITTED` 可见性。
- 说明与 MetadataNode、StorageNode、Client、ViewNode 自身的关系，保持 metadata/control-plane 与 StorageNode data-plane 边界清晰。
- 记录后续扩展点：多 ViewNode、自身高可用、注册租约、registry 持久化、认证授权和更细粒度诊断。

## 4. 不合理点 / 警告 / 风险

- 未发现需要扩大 T003 修改范围的文档冲突。
- 需要注意：后续如果实现多 ViewNode 或持久化 registry，必须先补充一致性语义和 Linux/Windows durability contract，否则容易把 ViewNode 误扩展为新的权威面。
- `tasks.md` 在本任务开始前已有 T001、T002 勾选改动；最终验证时又观察到 T004 也已被标记为 `[X]`。本任务只新增 T003 勾选，未修改 T004 对应文件，也未验证 T004。

## 5. common-risk-notes.md / risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 本任务仅创建 ViewNode 模块说明和目录级 agent 约束，风险已记录在本任务报告中。

## 6. 验证命令和结果

```bash
git diff -- modules/view/AGENTS.md modules/view/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t003-view-module-notes.md
```

结果：PASS，目标路径 diff 显示 `tasks.md` 已将 T003 标记为 `[X]`。

补充说明：`modules/view/` 和本报告文件为新增未跟踪文件，普通 `git diff -- <path>` 不展示其内容；已通过 `git status --short -- modules/view specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t003-view-module-notes.md` 确认新增文件和 `tasks.md` 修改范围。当前 `tasks.md` diff 中的 T001、T002 勾选是任务开始前已有改动；T004 勾选是在最终验证时观察到的并发/外部改动。本任务只新增 T003 勾选。

编译说明：本任务只修改 Markdown 文档和任务勾选，不新增业务代码、不修改 proto/CMake/测试，因此未运行 CMake 或测试。
