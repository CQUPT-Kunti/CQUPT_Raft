# T012 任务报告：durable node identity 接口定义

## 1. 修改了哪些文件

- `modules/cluster/node_identity.h`
- `modules/cluster/module-notes.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t012-node-identity-interface.md`

## 2. node_identity.h 定义了哪些身份类型和 load/store 接口边界

- 定义了稳定身份常量：
  - `kNodeIdentityCurrentVersion`
  - `kNodeIdentityFileName`
- 定义了身份来源、状态、诊断和持久化选项：
  - `NodeIdentitySource`
  - `NodeIdentityStatusCode`
  - `NodeIdentityIssueCode`
  - `NodeIdentityDurabilityMode`
  - `NodeIdentityStoreMode`
- 定义了核心身份结构：
  - `NodeIdentity`：表达 `cluster_id`、`node_id`、`node_type`、可选 `raft_id`、identity version、创建时间和来源。
  - `ExpectedNodeIdentity`：表达启动配置对本地身份的匹配期望，用于首次创建、重启复用和冲突诊断。
  - `NodeIdentityIssue` / `NodeIdentityValidationResult`：表达校验失败、配置 mismatch、corrupt identity、unsupported durability 等诊断边界。
- 定义了 load/store 输入和结果：
  - `NodeIdentityLoadOptions` / `NodeIdentityLoadResult`
  - `NodeIdentityStoreOptions` / `NodeIdentityStoreResult`
  - `NodeIdentityLoadOrCreateRequest` / `NodeIdentityLoadOrCreateResult`
- 声明了接口：
  - `ResolveNodeIdentityPath`
  - `ValidateNodeIdentity`
  - `ValidateNodeIdentityMatches`
  - `LoadNodeIdentity`
  - `StoreNodeIdentity`
  - `LoadOrCreateNodeIdentity`
  - `ToString(...)`
  - `DescribeNodeIdentityIssue`
- 接口边界明确：
  - MetadataNode 使用 `node_id` 表示集群身份，使用 `raft_id` 表示 Raft membership 身份。
  - StorageNode / ViewNode 不应携带 `raft_id`。
  - 已有 `node.identity` 与配置期望冲突时必须显式失败，不得静默覆盖。
  - durability mode 为 T013 的 Linux / Windows 实现预留状态表达。

## 3. 是否保持只定义接口、不实现持久化逻辑

- 是。
- `node_identity.h` 只包含类型、轻量 `ok()` 判断、函数声明和必要中文注释。
- 未实现文件解析、文件写入、临时文件、flush、atomic publish、directory durability 或 Windows 平台分支。
- 未新增 `modules/cluster/node_identity.cpp`，该实现留给 T013。

## 4. 是否发现不合理点 / 警告 / 风险

- `CMakeLists.txt` 已有 `modules/cluster/node_identity.cpp` 的 008 规划占位，但当前 CMake 会通过 existing-source 收集进行保护；T012 不创建 `.cpp`，后续 T013 负责实现。
- `tasks.md` 当前相对 HEAD 还显示 T010 / T015 的勾选变化；本轮补丁目标只包含 T012，未主动修改 T010 / T015 的任务状态。
- 本任务新增的是未来实现会使用的公共接口，T013 需要确保 required durability operation 在 Linux / Windows 上不能 no-op success。

## 5. 是否修改 common-risk-notes.md 或 risk-register.md

- 未修改 `common-risk-notes.md`。
- 未修改 `specs/008-integrated-object-storage-system/risk-register.md`。
- 原因：本次只定义 `node_identity.h` 接口和最小 module notes，不引入实际文件持久化逻辑、协议语义或新运行时行为。

## 6. 验证命令和结果

### 验证命令

```bash
git diff -- modules/cluster/node_identity.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t012-node-identity-interface.md
git status --short -- modules/cluster/node_identity.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t012-node-identity-interface.md
git diff --check -- modules/cluster/node_identity.h modules/cluster/module-notes.md specs/008-integrated-object-storage-system/tasks.md specs/008-integrated-object-storage-system/task-reports/t012-node-identity-interface.md
git diff --no-index --check /dev/null modules/cluster/node_identity.h || true
git diff --no-index --check /dev/null specs/008-integrated-object-storage-system/task-reports/t012-node-identity-interface.md || true
printf '#include "cluster/node_identity.h"\nint main() { return 0; }\n' | c++ -std=c++20 -I modules -x c++ -fsyntax-only -
```

### 验证结果

- `git diff --check` 无输出。
- 新增 `node_identity.h` 和本报告的 `git diff --no-index --check` 无输出。
- `c++ -std=c++20 -I modules -fsyntax-only` 通过，确认头文件 include、命名空间和语法边界合理。
- 未运行完整 CMake / build / test：本任务只新增尚未被实现文件引用的头文件接口，不涉及业务实现或链接行为。

## 结论

- T012 已完成。
- 从接口边界角度看，可以进入 T013。
