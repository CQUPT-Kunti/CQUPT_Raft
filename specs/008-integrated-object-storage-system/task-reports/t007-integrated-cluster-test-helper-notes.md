# T007 任务报告：integrated cluster test helper ownership notes

## 1. 修改了哪些文件

- `tests/support/integrated_cluster_test_utils.h`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t007-integrated-cluster-test-helper-notes.md`

## 2. integrated_cluster_test_utils.h 补充了什么 ownership notes

本次新建 `tests/support/integrated_cluster_test_utils.h`，仅补充零依赖的 ownership notes，没有加入真实测试逻辑、生产逻辑或额外链接依赖。

补充的边界包括：

- 允许后续承载：
  - 临时集群目录、临时文件、测试进程的 RAII 清理辅助
  - 测试用 `ClusterConfig` / `node.identity` 输入生成辅助
  - `ViewNode` / `MetadataNode` / `StorageNode` 的测试启动停止辅助
  - 测试端口分配、endpoint 组装与冲突规避辅助
  - 测试文件生成、SHA-256 计算与上传下载校验辅助
  - leader election、node registration、heartbeat / liveness 等待工具
  - StorageNode 故障、重启、checksum mismatch、quorum 不足等场景注入辅助

- 明确禁止：
  - helper 替代真实业务逻辑或演化成“测试专用控制面”
  - 绕过 Raft quorum / commit 直接制造 `COMMITTED` 对象
  - 伪造 StorageNode durable / flush / publish success
  - 把真实 payload / chunk bytes 写入 Raft log、snapshot 或 metadata helper
  - 把 ViewNode 注册结果解释为 Raft voter membership
  - 依赖固定端口、固定 `/tmp`、固定 Linux-only 路径或 shell 语义
  - 提前写入 T026 / T050 / T080 等后续任务的具体测试流程

- 维护约束：
  - 优先保持 header-only
  - 保持 Linux / Windows 路径与清理语义可诊断
  - 只承载跨多个测试文件复用的共享辅助

## 3. 是否发现不合理点 / 警告 / 风险

- `tests/support/` 当前已有多个主题化 helper 头，本次继续沿用“按测试主题拆分、避免万能工具箱”的风格，没有发现需要扩大修改范围的冲突。
- `integrated_cluster_test_utils.h` 当前只放 ownership notes，后续真正加入 helper 时仍需注意不要把等待逻辑、故障注入逻辑写成对 Linux shell、固定端口或固定临时目录的隐式依赖。
- `risk-register.md` 中已有 `R-007` 覆盖“真实多进程集成测试不稳定”的风险，本任务没有引入新的风险类型。

## 4. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次仅建立 helper ownership notes，没有新增设计边界，也没有改变已有测试行为。

## 5. 验证命令和结果

### 验证命令

```bash
git diff -- tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t007-integrated-cluster-test-helper-notes.md
git status --short -- tests/support/integrated_cluster_test_utils.h specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t007-integrated-cluster-test-helper-notes.md
```

### 验证结果

- `git diff -- ...` 按预期展示了 `tasks.md` 中 T007 从 `[ ]` 更新为 `[X]` 的修改。
- 由于 `tests/support/integrated_cluster_test_utils.h` 和本任务报告文件当前是未跟踪新文件，`git diff -- ...` 不会直接列出它们；补充执行 `git status --short -- ...` 后确认：
  - `M specs/008-integrated-object-storage-system/tasks.md`
  - `?? tests/support/integrated_cluster_test_utils.h`
  - `?? specs/008-integrated-object-storage-system/task-reports/t007-integrated-cluster-test-helper-notes.md`
- `integrated_cluster_test_utils.h` 只有 `#pragma once` 和中文 ownership notes，没有 include、没有未定义依赖、没有真实测试逻辑，因此不会单独引入编译或链接风险。

## 结论

- T007 已完成。
- 从 helper ownership notes 和任务勾选状态看，可以进入 T008。
