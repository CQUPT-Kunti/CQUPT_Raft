# T049 quickstart 命令示例更新

## 1. 修改了哪些文件

- `specs/008-integrated-object-storage-system/quickstart.md`
- `specs/008-integrated-object-storage-system/tasks.md`
- `specs/008-integrated-object-storage-system/task-reports/t049-quickstart-command-examples.md`

说明：

- 未修改 `apps/storage_client.cpp`
- 未修改 `apps/view_node_app.cpp`
- 未修改 `apps/metadata_node_app.cpp`
- 未修改 `apps/storage_node_app.cpp`
- 未修改 `CMakeLists.txt`
- 未修改测试文件
- `tasks.md` 当前工作区中同时包含前序任务 `T048` 的未提交勾选差异；本任务只额外把 `T049` 标记为完成，没有回滚该前置差异。

## 2. quickstart.md 更新了哪些命令示例

本任务把 `quickstart.md` 中的命令示例更新为当前仓库真实已实现的参数和 target 名称，主要包括：

- 更新构建命令：
  - 优先展示当前 quickstart 所需的最小 target 构建
  - 使用当前实际 target 名称：
    - `storage_client`
    - `view_node_app`
    - `metadata_node_app`
    - `storage_node_app`
  - 保留 `debug-ninja-safe` preset 作为主示例
- 更新 `storage_client generate-config` 示例：
  - 使用 `--base-dir`，不再使用旧的 `--base_dir`
  - 使用 `--view-count`
  - 使用 `--metadata-count`
  - 使用 `--metadata-voters`
  - 使用 `--storage-count`
  - 使用 `--cluster-id`
  - 使用 `--chunk-size`
  - 使用 `--replicas`
  - 使用 `--min-writes`
- 更新三个 node app 的启动示例：
  - `view_node_app --config ... --node_id ...`
  - `metadata_node_app --config ... --node_id ...`
  - `storage_node_app --config ... --node_id ...`
- 更新 `storage_client upload/download` 示例：
  - `upload` 使用：
    - `--config`
    - `--bucket`
    - `--object`
    - `--file`
  - `download` 使用：
    - `--config`
    - `--bucket`
    - `--object`
    - `--out`
- 增补 Windows 风格示例，保持参数名与 Linux 示例一致

## 3. 是否与当前 app 参数和 target 名称一致

一致。

本次更新已对照以下真实实现：

- `apps/storage_client.cpp`
- `apps/view_node_app.cpp`
- `apps/metadata_node_app.cpp`
- `apps/storage_node_app.cpp`
- `CMakeLists.txt`

已经修正的典型漂移包括：

- `--base_dir` -> `--base-dir`
- `--view_nodes` -> `--view-count`
- `--metadata_nodes` -> `--metadata-count`
- `--storage_nodes` -> `--storage-count`
- 可执行命令改为当前 target 对应名称：
  - `storage_client`
  - `view_node_app`
  - `metadata_node_app`
  - `storage_node_app`

## 4. 是否准确区分已实现流程和目标/待验证流程

已区分。

本次在 `quickstart.md` 中明确拆分了两类信息：

- 当前已实现的命令形态 / 参数边界
- 目标验收流程 / 当前限制 / 后续联调项

例如：

- `generate-config`、`upload`、`download`、三个 node app 启动命令，明确写成“当前已实现命令”。
- `ViewNode` 全量业务链路、`StorageNode -> ViewNode` registration / heartbeat loop、完整跨进程 discovery 联调、quorum safety 人工场景、StorageNode restart 恢复链路，明确写成“目标验收流程”或“后续联调项”，没有写成已经稳定可用。

## 5. 是否发现不合理点 / 警告 / 风险

- 旧版 `quickstart.md` 中存在多处参数名漂移，尤其是 cluster config 生成命令，已经和当前 CLI 实现不一致；本任务已修正。
- 当前 Linux 示例使用 `./build/linux/safe/<target>`，这是基于当前 preset 输出目录的仓库内惯例；Windows 部分为了避免写死未稳定的输出路径，只保留了 `.exe` 命令形态和 Windows 路径风格说明。
- `quickstart.md` 中的 upload/download 手动验收流程现在已准确反映命令接口，但是否能在当前分支完成完整多进程联调，仍要依赖后续 discovery / registration / heartbeat 等任务继续收敛。

## 6. 是否修改 common-risk-notes.md 或 risk-register.md

未修改 `common-risk-notes.md`。

未修改 `risk-register.md`。

## 7. 验证命令和结果

本任务是文档更新，默认未运行构建或 smoke build。

执行了以下对照/校验命令：

```bash
git diff -- specs/008-integrated-object-storage-system/quickstart.md \
  specs/008-integrated-object-storage-system/tasks.md \
  specs/008-integrated-object-storage-system/task-reports/t049-quickstart-command-examples.md
```

结果：

- `quickstart.md` 已更新
- `tasks.md` 仅将 `T049` 标记为完成
- 已生成本任务报告

执行了以下参数/target 名称对照检查：

```bash
rg -n -- '--base-dir|--view-count|--metadata-count|--metadata-voters|--storage-count|--bucket|--object |--file|--out |--config |--node_id|view_node_app|metadata_node_app|storage_node_app|storage_client' \
  specs/008-integrated-object-storage-system/quickstart.md \
  apps/storage_client.cpp \
  apps/view_node_app.cpp \
  apps/metadata_node_app.cpp \
  apps/storage_node_app.cpp \
  CMakeLists.txt
```

结果：

- quickstart 中的命令参数已与当前实现一致
- quickstart 中引用的 target 名称已与当前 `CMakeLists.txt` 一致
